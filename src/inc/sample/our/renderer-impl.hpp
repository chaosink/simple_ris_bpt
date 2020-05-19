#include <cstring>
///////////////////////////////////////////////////////////////////////////////////////////////////

namespace our{

///////////////////////////////////////////////////////////////////////////////////////////////////
//renderer
///////////////////////////////////////////////////////////////////////////////////////////////////


//constructor (M : number of pre-sampled light sub-paths, nt : number of threads)
inline renderer::renderer(const scene &scene, const camera &camera, const size_t M, const size_t nt) : m_M(M), m_nt(nt), m_sum(), m_ite()
{
	//number of samples for strategies (s>=1, t=1)
	m_ns1 = camera.res_x() * camera.res_y();

	//buffer to store contributions of strategies (s>=1, t=1)
	m_buf_s1 = imagef(camera.res_x(), camera.res_y());

	//spinlock
	m_locks = std::make_unique<spinlock[]>(camera.res_x() * camera.res_y());
}

///////////////////////////////////////////////////////////////////////////////////////////////////

//rendering
inline imagef renderer::render(const scene &scene, const camera &camera)
{
	m_ite += 1;

	const int w = camera.res_x();
	const int h = camera.res_y();
	imagef screen(w, h);

	//generate cache points (Line 3 of Algorithm1)
	{
		std::mutex mtx;
		std::vector<cache> caches;
		auto locked_add = [&](const camera_path_vertex &v){
			std::lock_guard<std::mutex> lock(mtx); caches.emplace_back(v, m_ite == 1);
		};

		//camera setup for generating cache points
		//generate eye sub-paths from camera_for_gen_caches (with approximately wxhx0.4% pixels)
		const float num = w * h * 0.004f;
		const int res_x = int(ceil(sqrt(num * camera.res_x() / float(camera.res_y()))));
		const int res_y = int(ceil(sqrt(num * camera.res_y() / float(camera.res_x()))));
		const ::camera camera_for_gen_caches(camera.p(), camera.p() + camera.d(), res_x, res_y, camera.fovy(), camera.lens_radius());

		//cache points are generated by tracing eye sub-paths. Each vertex of the eye sub-paths are used as the cache point
		std::atomic_size_t idx(0);
		in_parallel(res_x, res_y, [&](const int x, const int y)
		{
			thread_local random_number_generator rng(std::random_device{}());
			thread_local camera_path z;

			if(m_ite == 1){
                //for 1st iteration, estimate normalization factor Q using pre-sampled light sub-paths of 1st iteration
				z.construct(scene, camera_for_gen_caches, x, y, rng);
			}else{
                //estimate normalization factor Q using cache points at previous iteration (m_caches)
				z.construct(scene, camera_for_gen_caches, x, y, rng, m_caches);
			}
			for(size_t j = 1, n = z.num_vertices(); j < n; j++){
				locked_add(std::move(std::move(z(j)))); //generation of cache points for current iteration
			}
		}, m_nt);

		//construct kd-tree to search cache points
		m_caches = kd_tree<cache>(std::move(caches), [](const cache &c) -> const vec3&{
			return c.intersection().p();
		});
	}

	//generate light sub-paths
	//we prepare wxh light sub-paths and each light sub-path is used for strategies other than resampling strategies.
	m_light_paths.resize(w * h);
	in_parallel(w * h, [&](const int idx)
	{
		thread_local random_number_generator rng(std::random_device{}());
		m_light_paths[idx].construct(scene, rng, m_caches);
	}, m_nt);

	//generate ¥hat{Y}_n in Line 2 of Algorithm1
	{
		size_t V = 0;
		for(size_t i = 0; i < m_M; i++){
			V += m_light_paths[i].num_vertices();
		}
		m_candidates.resize(V);

		V = 0;
		for(size_t i = 0; i < m_M; i++){
			for(size_t j = 0, n = m_light_paths[i].num_vertices(); j < n; j++){
				m_candidates[V++] = candidate(m_light_paths[i], j);
			}
		}
	}

	//construct resampling pmfs at cache points
	in_parallel(int(m_caches.end() - m_caches.begin()), [&](const int idx)
	{
		const cache &c = *(m_caches.begin() + idx);
		const_cast<cache&>(c).calc_distribution(scene, m_candidates, m_M);
	}, m_nt);

	//calculate normalization factor for virtual cache point
	{
		m_sum += m_candidates.size() / double(m_M);
		m_Qp = float(m_sum / m_ite);
	}

	//initialize buffer that stores contributions of strategies (s>=1,t=1) of light tracing
	memset(m_buf_s1(0,0), 0, sizeof(float) * 3 * w * h);

	in_parallel(w, h, [&](const int x, const int y)
	{
		thread_local random_number_generator rng(std::random_device{}());

		const col3 col = radiance(x, y, scene, camera, rng);
		if(!(std::isnan(col[0] + col[1] + col[2]))){
			screen(x, y)[0] = col[0];
			screen(x, y)[1] = col[1];
			screen(x, y)[2] = col[2];
		}
	}, m_nt);

	//add contributions of strategies (s>=1,t=1)
	const float inv_ns1 = 1 / float(m_ns1);
	for(int y = 0; y < h; y++){
		for(int x = 0; x < w; x++){
			screen(x, y)[0] += m_buf_s1(x, y)[0] * inv_ns1;
			screen(x, y)[1] += m_buf_s1(x, y)[1] * inv_ns1;
			screen(x, y)[2] += m_buf_s1(x, y)[2] * inv_ns1;
		}
	}
	return screen;
}

///////////////////////////////////////////////////////////////////////////////////////////////////

//radiance calculation (x,y: pixel coordinate)
inline col3 renderer::radiance(const int x, const int y, const scene &scene, const camera &camera, random_number_generator &rng)
{
	thread_local camera_path camera_path;

	//generate eye sub-path
	camera_path.construct(scene, camera, x, y, rng, m_caches);
	const light_path &light_path = m_light_paths[x + camera.res_x() * y];

	//calculate contributions of strategies (s>=1,t=1) and store them in m_buf_s1
	calculate_s1(scene, camera, light_path, camera_path, rng);

	//calculate contributions of resampling strategies (s>=1,t>=2) and strategies (s=0,t>=2)
	return calculate_0t(scene, light_path, camera_path) + calculate_st(scene, camera_path, rng);
}

///////////////////////////////////////////////////////////////////////////////////////////////////

//calculate contributions of strategies (s=0,t>=2) (unidirectional path tracing)
inline col3 renderer::calculate_0t(const scene &scene, const light_path &y, const camera_path &z)
{
	const size_t t = z.num_vertices();
	if(t >= 2){

		const auto &ztm1 = z(t - 1);
		const auto &ztm1_isect = z(t - 1).intersection();

		//if z(t-1) is on light source
		if(ztm1_isect.material().is_emissive()){

			const col3 Le = ztm1_isect.material().Le(ztm1_isect, ztm1.wo());

			const float mis_weight = 1 / (
				0 + 1 + camera_path::mis_partial_weight(scene, y, 0, z, t, direction(), ztm1.wi(), m_M, m_Qp)
			);
			return Le * ztm1.throughput_We() * mis_weight;
		}
	}
	return col3();
}

///////////////////////////////////////////////////////////////////////////////////////////////////

//calculate contributions of strategies (s>=1, t=1)
inline void renderer::calculate_s1(const scene &scene, const camera &camera, const light_path &y, const camera_path &z, random_number_generator &rng)
{
	for(size_t s = 1, nL = y.num_vertices(); s <= nL; s++){
		
		const auto &z0 = z(0);
		const auto &ysm1 = y(s - 1);
		const auto &z0_isect = z(0).intersection();
		const auto &ysm1_isect = y(s - 1).intersection();

		const vec3 tmp_zy = ysm1_isect.p() - z0_isect.p();
		const float dist2 = squared_norm(tmp_zy);
		const float dist = sqrt(dist2);
		const direction zy(tmp_zy / dist, z0_isect.n());
		if(zy.is_invalid() || zy.in_lower_hemisphere()){
			continue;
		}

		const direction yz(-zy, ysm1_isect.n());
		if(yz.is_invalid() || yz.in_lower_hemisphere()){
			continue;
		}

		//calculate intersection on screen
		const auto screen_pos = camera.calc_intersection(z0_isect.p(), zy);
		if(screen_pos.is_valid){

			//visibility test
			if(scene.intersect(ray(z0_isect.p(), zy, dist)) == false){

				const col3 fyz = ysm1.brdf().f(yz);
				const float We = camera.We(zy);
				const float G = yz.abs_cos() * zy.abs_cos() / dist2;

				const float mis_weight = m_ns1 / (
					light_path::mis_partial_weight(y, s, z, 1, yz, zy, m_M, m_Qp) + m_ns1 + 0
				);
				const col3 contrib = ysm1.Le_throughput() * fyz * (We * G / z0.pdf_fwd() * mis_weight);

				//update m_buf_s1 using spinlock
				std::lock_guard<spinlock> lock(m_locks[screen_pos.x + camera.res_x() * screen_pos.y]);
				m_buf_s1(screen_pos.x, screen_pos.y)[0] += contrib[0];
				m_buf_s1(screen_pos.x, screen_pos.y)[1] += contrib[1];
				m_buf_s1(screen_pos.x, screen_pos.y)[2] += contrib[2];
			}
		}
	}
}

///////////////////////////////////////////////////////////////////////////////////////////////////

//calculate contributions for resampling estimators
inline col3 renderer::calculate_st(const scene &scene, const camera_path &z, random_number_generator &rng)
{
	const size_t nE = z.num_vertices();

	col3 L;
	for(size_t t = 2; t <= nE; t++){

		const auto &ztm1 = z(t - 1);
		const auto &ztm1_isect = z(t - 1).intersection();

		if(ztm1_isect.material().is_emissive()){
			continue;
		}

		//sample cache point uniformly (i.e, P_c(i)=1/(Nc+1) in Sec. 5.2)
		size_t cache_idx = Nc;
		float pmf = 1 / float(Nc + 1);
		{
			float u = rng.generate_uniform_real();
			for(size_t i = 0; i < Nc; i++){
				if(u < 1 / float(Nc + 1)){
					cache_idx = i; break;
				}
				u -= 1 / float(Nc + 1);
			}
		}
		if((cache_idx != Nc) && (ztm1.neighbor_cache(cache_idx).normalization_constant() == 0)){
			continue;
		}

		//resample light sub-path  (Line13 in Algorithm1)
		size_t sample_idx;
		const candidate *p_candidate;
		if(cache_idx != Nc){
			const auto sample = ztm1.neighbor_cache(cache_idx).sample(rng);
			sample_idx = sample.p_elem - &*ztm1.neighbor_cache(cache_idx).begin();
			p_candidate = sample.p_elem;
			pmf *= sample.pmf;
		}else{
		    //use virtual cache point
			sample_idx = rng.generate_uniform_int(0, m_candidates.size() - 1);
			p_candidate = &m_candidates[sample_idx];
			pmf *= 1 / float(m_candidates.size());
		}
		const auto &y = p_candidate->path();
		const auto  s = p_candidate->s();
		const auto &ysm1 = y(s - 1);
		const auto &ysm1_isect = y(s - 1).intersection();

		const vec3 tmp_yz = ztm1_isect.p() - ysm1_isect.p();
		const float dist2 = squared_norm(tmp_yz);
		const float dist = sqrt(dist2);
		const direction yz(tmp_yz / dist, ysm1_isect.n());
		if(yz.is_invalid() || yz.in_lower_hemisphere()){
			continue;
		}

		const direction zy(-yz, ztm1_isect.n());
		if(zy.is_invalid() || zy.in_lower_hemisphere()){
			continue;
		}

		//visibility test between y(s-1) & z(t-1)
		if(scene.intersect(ray(ysm1_isect.p(), yz, dist)) == false){

			const col3 fyz = ysm1.brdf().f(yz);
			const col3 fzy = ztm1.brdf().f(zy);
			const float G = yz.abs_cos() * zy.abs_cos() / dist2;

			//calculate resampling-aware weighting function
			float mis_weight;
			{
				float val, sum_val = 0;
				for(size_t i = 0; i < Nc; i++){
				    //normalization factor Q at nearest cache point
					const float Q = ztm1.neighbor_cache(i).Q();

					//calculate q*/p
					const float Le_throughput_FGVc = ztm1.neighbor_cache(i).pmf(sample_idx) * ztm1.neighbor_cache(i).normalization_constant();
				
					if(Le_throughput_FGVc > 0){
						const float tmp_val = (1 / float(Nc + 1)) * m_M / (
							(m_M - 1) * std::max(mis_threshold, Q / Le_throughput_FGVc) + 1
						);
						if(cache_idx == i){
							val = tmp_val;
						}
						sum_val += tmp_val;
					}
				}
				const float tmp_val = (1 / float(Nc + 1)) * m_M / (
					(m_M - 1) * m_Qp + 1
				);
				if(cache_idx == Nc){
					val = tmp_val;
				}
				sum_val += tmp_val;
				
				mis_weight = val / (
					light_path::mis_partial_weight(y, s, z, t, yz, zy, m_M, m_Qp) + sum_val + camera_path::mis_partial_weight(scene, y, s, z, t, yz, zy, m_M, m_Qp)
				);
			}

			L += ysm1.Le_throughput() * fyz * fzy * ztm1.throughput_We() * (G / (pmf * m_M) * mis_weight);
		}
	}
	return L;
}

///////////////////////////////////////////////////////////////////////////////////////////////////

} //namespace our

///////////////////////////////////////////////////////////////////////////////////////////////////
