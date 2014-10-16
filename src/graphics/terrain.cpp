#include "terrain.h"
#include "core/aabb.h"
#include "core/frustum.h"
#include "core/iserializer.h"
#include "core/log.h"
#include "core/math_utils.h"
#include "core/profiler.h"
#include "core/resource_manager.h"
#include "engine/engine.h"
#include "graphics/geometry.h"
#include "graphics/material.h"
#include "graphics/model.h"
#include "graphics/pipeline.h"
#include "graphics/render_scene.h"
#include "graphics/renderer.h"
#include "graphics/shader.h"
#include "graphics/texture.h"
#include <cfloat>


namespace Lumix
{
	
	static const float GRASS_QUAD_RADIUS = Terrain::GRASS_QUAD_SIZE * 0.7072f;
	static const int GRID_SIZE = 16;
	static const int COPY_COUNT = 50;
	static const uint32_t TERRAIN_HASH = crc32("terrain");
	static const uint32_t MORPH_CONST_HASH = crc32("morph_const");
	static const uint32_t QUAD_SIZE_HASH = crc32("quad_size");
	static const uint32_t QUAD_MIN_HASH = crc32("quad_min");

	static const uint32_t BRUSH_POSITION_HASH = crc32("brush_position");
	static const uint32_t BRUSH_SIZE_HASH = crc32("brush_size");
	static const uint32_t MAP_SIZE_HASH = crc32("map_size");
	static const uint32_t CAMERA_POS_HASH = crc32("camera_pos");

	struct Sample
	{
		Vec3 pos;
		float u, v;
	};

	struct TerrainQuad
	{
		enum ChildType
		{
			TOP_LEFT,
			TOP_RIGHT,
			BOTTOM_LEFT,
			BOTTOM_RIGHT,
			CHILD_COUNT
		};

		TerrainQuad()
		{
			for (int i = 0; i < CHILD_COUNT; ++i)
			{
				m_children[i] = NULL;
			}
		}

		~TerrainQuad()
		{
			for (int i = 0; i < CHILD_COUNT; ++i)
			{
				LUMIX_DELETE(m_children[i]);
			}
		}

		void createChildren()
		{
			if (m_lod < 16 && m_size > 16)
			{
				for (int i = 0; i < CHILD_COUNT; ++i)
				{
					m_children[i] = LUMIX_NEW(TerrainQuad);
					m_children[i]->m_lod = m_lod + 1;
					m_children[i]->m_size = m_size / 2;
				}
				m_children[TOP_LEFT]->m_min = m_min;
				m_children[TOP_RIGHT]->m_min.set(m_min.x + m_size / 2, 0, m_min.z);
				m_children[BOTTOM_LEFT]->m_min.set(m_min.x, 0, m_min.z + m_size / 2);
				m_children[BOTTOM_RIGHT]->m_min.set(m_min.x + m_size / 2, 0, m_min.z + m_size / 2);
				for (int i = 0; i < CHILD_COUNT; ++i)
				{
					m_children[i]->createChildren();
				}
			}
		}

		float getSquaredDistance(const Vec3& camera_pos)
		{
			Vec3 _max(m_min.x + m_size, m_min.y, m_min.z + m_size);
			float dist = 0;
			if (camera_pos.x < m_min.x)
			{
				float d = m_min.x - camera_pos.x;
				dist += d*d;
			}
			if (camera_pos.x > _max.x)
			{
				float d = _max.x - camera_pos.x;
				dist += d*d;
			}
			if (camera_pos.z < m_min.z)
			{
				float d = m_min.z - camera_pos.z;
				dist += d*d;
			}
			if (camera_pos.z > _max.z)
			{
				float d = _max.z - camera_pos.z;
				dist += d*d;
			}
			return dist;
		}

		static float getRadiusInner(float size)
		{
			float lower_level_size = size / 2;
			float lower_level_diagonal = sqrt(2 * size / 2 * size / 2);
			return getRadiusOuter(lower_level_size) + lower_level_diagonal;
		}

		static float getRadiusOuter(float size)
		{
			return (size > 17 ? 2 : 1) * sqrt(2 * size*size) + size * 0.25f;
		}


		bool render(Renderer* renderer, Mesh* mesh, Geometry& geometry, const Vec3& camera_pos, RenderScene& scene)
		{
			PROFILE_FUNCTION();
			float squared_dist = getSquaredDistance(camera_pos);
			float r = getRadiusOuter(m_size);
			if (squared_dist > r*r && m_lod > 1)
			{
				return false;
			}
			Vec3 morph_const(r, getRadiusInner(m_size), 0);
			Shader& shader = *mesh->getMaterial()->getShader();
			for (int i = 0; i < CHILD_COUNT; ++i)
			{
				if (!m_children[i] || !m_children[i]->render(renderer, mesh, geometry, camera_pos, scene))
				{
					setFixedCachedUniform(*renderer, shader, (int)Shader::FixedCachedUniforms::MORPH_CONST, morph_const);
					setFixedCachedUniform(*renderer, shader, (int)Shader::FixedCachedUniforms::QUAD_SIZE, m_size);
					setFixedCachedUniform(*renderer, shader, (int)Shader::FixedCachedUniforms::QUAD_MIN, m_min);
					renderGeometry(*renderer, geometry, mesh->getCount() / 4 * i, mesh->getCount() / 4, shader);
				}
			}
			return true;
		}

		TerrainQuad* m_children[CHILD_COUNT];
		Vec3 m_min;
		float m_size;
		int m_lod;
	};


	Terrain::Terrain(const Entity& entity, RenderScene& scene)
		: m_mesh(NULL)
		, m_material(NULL)
		, m_root(NULL)
		, m_width(0)
		, m_height(0)
		, m_layer_mask(1)
		, m_y_scale(1)
		, m_xz_scale(1)
		, m_entity(entity)
		, m_scene(scene)
		, m_brush_position(0, 0, 0)
		, m_brush_size(1)
	{
		generateGeometry();
	}

	Terrain::GrassType::~GrassType()
	{
		if (m_grass_model)
		{
			m_grass_model->getResourceManager().get(ResourceManager::MODEL)->unload(*m_grass_model);
			m_grass_model->getObserverCb().unbind<GrassType, &GrassType::grassLoaded>(this);
			LUMIX_DELETE(m_grass_mesh);
			LUMIX_DELETE(m_grass_geometry);
		}
	}

	Terrain::~Terrain()
	{
		setMaterial(NULL);
		LUMIX_DELETE(m_mesh);
		LUMIX_DELETE(m_root);
		for(int i = 0; i < m_grass_types.size(); ++i)
		{
			LUMIX_DELETE(m_grass_types[i]);
		}
		for(Map<Component, Array<GrassQuad*> >::iterator iter = m_grass_quads.begin(), end = m_grass_quads.end(); iter != end; ++iter)
		{
			Array<GrassQuad*>& quads = iter.value();
			for (int i = 0; i < quads.size(); ++i)
			{
				LUMIX_DELETE(quads[i]);
			}
		}
		for (int i = 0; i < m_free_grass_quads.size(); ++i)
		{
			LUMIX_DELETE(m_free_grass_quads[i]);
		}
	}


	Terrain::GrassType::GrassType(Terrain& terrain)
		: m_terrain(terrain)
	{
		m_grass_geometry = NULL;
		m_grass_mesh = NULL;
		m_grass_model = NULL;
		m_ground = 0;
		m_density = 10;
	}


	void Terrain::addGrassType(int index)
	{
		if(index < 0)
		{
			m_grass_types.push(LUMIX_NEW(GrassType)(*this));
		}
		else
		{
			m_grass_types.insert(index, LUMIX_NEW(GrassType)(*this));
		}
	}


	void Terrain::removeGrassType(int index)
	{
		forceGrassUpdate();
		LUMIX_DELETE(m_grass_types[index]);
		m_grass_types.erase(index);
	}


	void Terrain::setGrassTypeDensity(int index, int density)
	{
		forceGrassUpdate();
		GrassType& type = *m_grass_types[index];
		type.m_density = Math::clamp(density, 0, 50);
	}


	int Terrain::getGrassTypeDensity(int index)
	{
		GrassType& type = *m_grass_types[index];
		return type.m_density;
	}


	void Terrain::setGrassTypeGround(int index, int ground)
	{
		ground = Math::clamp(ground, 0, 3);
		forceGrassUpdate();
		GrassType& type = *m_grass_types[index];
		type.m_ground = ground;
	}
	
	
	int Terrain::getGrassTypeGround(int index)
	{
		GrassType& type = *m_grass_types[index];
		return type.m_ground;
	}


	Path Terrain::getGrassTypePath(int index)
	{
		GrassType& type = *m_grass_types[index];
		if (type.m_grass_model)
		{
			return type.m_grass_model->getPath();
		}
		return "";
	}


	void Terrain::setGrassTypePath(int index, const Path& path)
	{
		forceGrassUpdate();
		GrassType& type = *m_grass_types[index];
		if (type.m_grass_model)
		{
			type.m_grass_model->getResourceManager().get(ResourceManager::MODEL)->unload(*type.m_grass_model);
			type.m_grass_model->getObserverCb().unbind<GrassType, &GrassType::grassLoaded>(&type);
			type.m_grass_model = NULL;
			LUMIX_DELETE(type.m_grass_mesh);
			LUMIX_DELETE(type.m_grass_geometry);
			type.m_grass_mesh = NULL;
			type.m_grass_geometry = NULL;
		}
		if (path.isValid())
		{
			type.m_grass_model = static_cast<Model*>(m_scene.getEngine().getResourceManager().get(ResourceManager::MODEL)->load(path));
			type.m_grass_model->getObserverCb().bind<GrassType, &GrassType::grassLoaded>(&type);
			if(type.m_grass_model->isReady())
			{
				type.grassLoaded(Resource::State::READY, Resource::State::READY);
			}
		}
	}
	

	void Terrain::forceGrassUpdate()
	{
		m_force_grass_update = true;
		for(Map<Component, Array<GrassQuad*> >::iterator iter = m_grass_quads.begin(), end = m_grass_quads.end(); iter != end; ++iter)
		{
			Array<GrassQuad*>& quads = iter.value();
			while(!quads.empty())
			{
				m_free_grass_quads.push(quads.back());
				quads.pop();
			}
		}
	}


	void Terrain::updateGrass(const Component& camera)
	{
		PROFILE_FUNCTION();
		if (!m_splatmap)
		{
			return;
		}
		if (m_free_grass_quads.size() + m_grass_quads.size() < GRASS_QUADS_ROWS * GRASS_QUADS_COLUMNS)
		{
			int new_count = GRASS_QUADS_ROWS * GRASS_QUADS_COLUMNS - m_grass_quads.size();
			for (int i = 0; i < new_count; ++i)
			{
				m_free_grass_quads.push(LUMIX_NEW(GrassQuad));
			}
		}

		Vec3 camera_position = camera.entity.getPosition();
		if ((m_last_camera_position[camera] - camera_position).length() > 1 || m_force_grass_update)
		{
			m_force_grass_update = false;
			Matrix mtx = m_entity.getMatrix();
			Matrix inv_mtx = m_entity.getMatrix();
			inv_mtx.fastInverse();
			Vec3 local_camera_position = inv_mtx.multiplyPosition(camera_position);
			float cx = (int)(local_camera_position.x / (GRASS_QUAD_SIZE)) * (float)GRASS_QUAD_SIZE;
			float cz = (int)(local_camera_position.z / (GRASS_QUAD_SIZE)) * (float)GRASS_QUAD_SIZE;
			float from_quad_x = cx - (GRASS_QUADS_COLUMNS >> 1) * GRASS_QUAD_SIZE;
			float from_quad_z = cz - (GRASS_QUADS_ROWS >> 1) * GRASS_QUAD_SIZE;
			float to_quad_x = cx + (GRASS_QUADS_COLUMNS >> 1) * GRASS_QUAD_SIZE;
			float to_quad_z = cz + (GRASS_QUADS_COLUMNS >> 1) * GRASS_QUAD_SIZE;

			float old_bounds[4] = { FLT_MAX, -FLT_MAX, FLT_MAX, -FLT_MAX };
			Array<GrassQuad*>& quads = m_grass_quads[camera];
			for (int i = quads.size() - 1; i >= 0; --i)
			{
				GrassQuad* quad = quads[i];
				old_bounds[0] = Math::minValue(old_bounds[0], quad->m_x);
				old_bounds[1] = Math::maxValue(old_bounds[1], quad->m_x);
				old_bounds[2] = Math::minValue(old_bounds[2], quad->m_z);
				old_bounds[3] = Math::maxValue(old_bounds[3], quad->m_z);
				if (quad->m_x < from_quad_x || quad->m_x > to_quad_x || quad->m_z < from_quad_z || quad->m_z > to_quad_z)
				{
					m_free_grass_quads.push(quads[i]);
					quads.eraseFast(i);
				}
			}

			from_quad_x = Math::maxValue(0.0f, from_quad_x);
			from_quad_z = Math::maxValue(0.0f, from_quad_z);

			for (float quad_z = from_quad_z; quad_z <= to_quad_z; quad_z += GRASS_QUAD_SIZE)
			{
				for (float quad_x = from_quad_x; quad_x <= to_quad_x; quad_x += GRASS_QUAD_SIZE)
				{
					if (quad_x < old_bounds[0] || quad_x > old_bounds[1] || quad_z < old_bounds[2] || quad_z > old_bounds[3])
					{
						GrassQuad* quad = NULL;
						if(!m_free_grass_quads.empty())
						{
							quad = m_free_grass_quads.back();
							m_free_grass_quads.pop();
						}
						else
						{
							quad = LUMIX_NEW(GrassQuad);
						}
						quads.push(quad);
						quad->m_x = quad_x;
						quad->m_z = quad_z;
						quad->m_patches.clear();
						srand((int)quad_x + (int)quad_z * GRASS_QUADS_COLUMNS);
						for(int grass_type_idx = 0; grass_type_idx < m_grass_types.size(); ++grass_type_idx)
						{
							GrassPatch& patch = quad->m_patches.pushEmpty();
							patch.m_matrices.clear();
							patch.m_type = m_grass_types[grass_type_idx];
							if(patch.m_type->m_grass_geometry)
							{
								int index = 0;
								Texture* splat_map = m_splatmap;
								float step = GRASS_QUAD_SIZE / (float)patch.m_type->m_density;
								for (float dx = 0; dx < GRASS_QUAD_SIZE; dx += step)
								{
									for (float dz = 0; dz < GRASS_QUAD_SIZE; dz += step)
									{
										uint32_t pixel_value = splat_map->getPixel(splat_map->getWidth() * (quad_x + dx) / m_width, splat_map->getHeight() * (quad_z + dz) / m_height);
										uint8_t count = (pixel_value >> (8 * patch.m_type->m_ground)) & 0xff;
										float density = count / 255.0f;
										if(density > 0.25f)
										{
											Matrix& grass_mtx = patch.m_matrices.pushEmpty();
											grass_mtx = Matrix::IDENTITY;
											float x = quad_x + dx + step * (rand() % 100 - 50) / 100.0f;
											float z = quad_z + dz + step * (rand() % 100 - 50) / 100.0f;
											grass_mtx.setTranslation(Vec3(x, getHeight(x, z), z));
											grass_mtx = mtx * grass_mtx;
											grass_mtx.multiply3x3(density + (rand() % 20 - 10) / 100.0f);
										}

										++index;
									}
								}
							}
						}
					}
				}
			}
			m_last_camera_position[camera] = camera_position;
		}
	}


	void Terrain::GrassType::grassVertexCopyCallback(Array<uint8_t>& data)
	{
		bool has_matrix_index_attribute = m_grass_model->getGeometry()->getVertexDefinition().getAttributeType(3) == VertexAttributeDef::INT1;
		if (has_matrix_index_attribute)
		{
			int vertex_size = m_grass_model->getGeometry()->getVertexDefinition().getVertexSize();
			int one_size = vertex_size * m_grass_model->getGeometry()->getVertices().size();
			const int i1_offset = 3 * sizeof(float) + 3 * sizeof(float) + 2 * sizeof(float);
			for (int i = 0; i < COPY_COUNT; ++i)
			{
				for (int j = 0; j < m_grass_model->getGeometry()->getVertices().size(); ++j)
				{
					data[i * one_size + j * vertex_size + i1_offset] = (uint8_t)i;
				}
			}
		}
		else
		{
			g_log_error.log("renderer") << "Mesh " << m_grass_model->getPath().c_str() << " is not a grass mesh - wrong format";
		}
	}


	void Terrain::GrassType::grassIndexCopyCallback(Array<int>& data)
	{
		int indices_count = m_grass_model->getGeometry()->getIndices().size();
		int index_offset = m_grass_model->getGeometry()->getVertices().size();
		for (int i = 0; i < COPY_COUNT; ++i)
		{
			for (int j = 0, c = indices_count; j < c; ++j)
			{
				data[i * indices_count + j] += index_offset * i;
			}
		}
	}


	void Terrain::GrassType::grassLoaded(Resource::State, Resource::State)
	{
		if (m_grass_model->isReady())
		{
			LUMIX_DELETE(m_grass_geometry);

			m_grass_geometry = LUMIX_NEW(Geometry);
			Geometry::VertexCallback vertex_callback;
			Geometry::IndexCallback index_callback;
			vertex_callback.bind<GrassType, &GrassType::grassVertexCopyCallback>(this);
			index_callback.bind<GrassType, &GrassType::grassIndexCopyCallback>(this);
			m_grass_geometry->copy(*m_grass_model->getGeometry(), COPY_COUNT, vertex_callback, index_callback);
			Material* material = m_grass_model->getMesh(0).getMaterial();
			m_grass_mesh = LUMIX_NEW(Mesh)(material, 0, m_grass_model->getMesh(0).getCount() * COPY_COUNT, "grass");
			m_terrain.forceGrassUpdate();
		}
	}


	void Terrain::getGrassInfos(const Frustum& frustum, Array<GrassInfo>& infos, const Component& camera)
	{
		updateGrass(camera);
		Array<GrassQuad*>& quads = m_grass_quads[camera];
		for (int i = 0; i < quads.size(); ++i)
		{
			Vec3 quad_center(quads[i]->m_x + GRASS_QUAD_SIZE * 0.5f, 0, quads[i]->m_z + GRASS_QUAD_SIZE * 0.5f);
			if(frustum.isSphereInside(quad_center, GRASS_QUAD_RADIUS))
			{
				for(int patch_idx = 0; patch_idx < quads[i]->m_patches.size(); ++patch_idx)
				{
					GrassPatch& patch = quads[i]->m_patches[patch_idx];
					for (int k = 0, kc = patch.m_matrices.size() / COPY_COUNT; k < kc; ++k)
					{
						GrassInfo& info = infos.pushEmpty();
						info.m_geometry = patch.m_type->m_grass_geometry;
						info.m_matrices = &patch.m_matrices[COPY_COUNT * k];
						info.m_mesh = patch.m_type->m_grass_mesh;
						info.m_matrix_count = COPY_COUNT;
						info.m_mesh_copy_count = COPY_COUNT;
					}
					if (patch.m_matrices.size() % COPY_COUNT != 0)
					{
						GrassInfo& info = infos.pushEmpty();
						info.m_geometry = patch.m_type->m_grass_geometry;
						info.m_matrices = &patch.m_matrices[COPY_COUNT * (patch.m_matrices.size() / COPY_COUNT)];
						info.m_mesh = patch.m_type->m_grass_mesh;
						info.m_matrix_count = patch.m_matrices.size() % COPY_COUNT;
						info.m_mesh_copy_count = COPY_COUNT;
					}
				}
			}
		}
	}


	void Terrain::setMaterial(Material* material)
	{
		if (material != m_material)
		{
			if (m_material)
			{
				m_material->getResourceManager().get(ResourceManager::MATERIAL)->unload(*m_material);
				m_material->getObserverCb().unbind<Terrain, &Terrain::onMaterialLoaded>(this);
			}
			m_material = material;
			m_splatmap = NULL;
			m_heightmap = NULL;
			if (m_mesh && m_material)
			{
				m_mesh->setMaterial(m_material);
				m_material->getObserverCb().bind<Terrain, &Terrain::onMaterialLoaded>(this);
				if (m_material->isReady())
				{
					onMaterialLoaded(Resource::State::READY, Resource::State::READY);
				}
			}
		}
		else if(material)
		{
			material->getResourceManager().get(ResourceManager::MATERIAL)->unload(*material);
		}
	}

	void Terrain::deserialize(ISerializer& serializer, Universe& universe, RenderScene& scene, int index)
	{
		serializer.deserializeArrayItem(m_entity.index);
		m_entity.universe = &universe;
		serializer.deserializeArrayItem(m_layer_mask);
		char path[LUMIX_MAX_PATH];
		serializer.deserializeArrayItem(path, LUMIX_MAX_PATH);
		setMaterial(static_cast<Material*>(scene.getEngine().getResourceManager().get(ResourceManager::MATERIAL)->load(path)));
		serializer.deserializeArrayItem(m_xz_scale);
		serializer.deserializeArrayItem(m_y_scale);
		int count;
		serializer.deserializeArrayItem(count);
		while(m_grass_types.size() > count)
		{
			removeGrassType(m_grass_types.size() - 1);
		}

		while(m_grass_types.size() < count)
		{
			addGrassType(-1);
		}
		for(int i = 0; i < count; ++i)
		{
			serializer.deserializeArrayItem(path, LUMIX_MAX_PATH);
			serializer.deserializeArrayItem(m_grass_types[i]->m_ground);
			serializer.deserializeArrayItem(m_grass_types[i]->m_density);
			setGrassTypePath(i, path);
		}
		universe.addComponent(m_entity, TERRAIN_HASH, &scene, index);
	}


	void Terrain::serialize(ISerializer& serializer)
	{
		serializer.serializeArrayItem(m_entity.index);
		serializer.serializeArrayItem(m_layer_mask);
		serializer.serializeArrayItem(m_material ? m_material->getPath().c_str() : "");
		serializer.serializeArrayItem(m_xz_scale);
		serializer.serializeArrayItem(m_y_scale);
		serializer.serializeArrayItem(m_grass_types.size());
		for(int i = 0; i < m_grass_types.size(); ++i)
		{
			GrassType& type = *m_grass_types[i];
			serializer.serializeArrayItem(type.m_grass_model ? type.m_grass_model->getPath().c_str() : "");
			serializer.serializeArrayItem(type.m_ground);
			serializer.serializeArrayItem(type.m_density);

		}
	}


	void Terrain::render(Renderer& renderer, PipelineInstance& pipeline, const Vec3& camera_pos)
	{
		if (m_root)
		{
			m_material->apply(renderer, pipeline);
			Matrix inv_world_matrix;
			Matrix world_matrix;
			m_entity.getMatrix(world_matrix);
			inv_world_matrix = world_matrix;
			inv_world_matrix.fastInverse();
			Vec3 rel_cam_pos = inv_world_matrix.multiplyPosition(camera_pos) / m_xz_scale;
			Shader& shader = *m_mesh->getMaterial()->getShader();
			renderer.setUniform(shader, "brush_position", BRUSH_POSITION_HASH, m_brush_position);
			renderer.setUniform(shader, "brush_size", BRUSH_SIZE_HASH, m_brush_size);
			renderer.setUniform(shader, "map_size", MAP_SIZE_HASH, m_root->m_size);
			renderer.setUniform(shader, "camera_pos", CAMERA_POS_HASH, rel_cam_pos);
			m_root->render(&static_cast<Renderer&>(m_scene.getPlugin()), m_mesh, m_geometry, rel_cam_pos, *pipeline.getScene());
		}
	}

	float Terrain::getHeight(float x, float z)
	{
		int int_x = (int)x;
		int int_z = (int)z;
		float dec_x = x - int_x;
		float dec_z = z - int_z;
		if (dec_x > dec_z)
		{
			float h0 = getHeight(int_x, int_z);
			float h1 = getHeight(int_x + 1, int_z);
			float h2 = getHeight(int_x + 1, int_z + 1);
			return h0 + (h1 - h0) * dec_x + (h2 - h1) * dec_z;
		}
		else
		{
			float h0 = getHeight(int_x, int_z);
			float h1 = getHeight(int_x + 1, int_z + 1);
			float h2 = getHeight(int_x, int_z + 1);
			return h0 + (h2 - h0) * dec_z + (h1 - h2) * dec_x;
		}
	}
	

	float Terrain::getHeight(int x, int z)
	{
		int texture_x = (int)(x / m_xz_scale);
		int texture_y = (int)(z / m_xz_scale);
		Texture* t = m_heightmap;
		int idx = Math::clamp(texture_x, 0, m_width) + Math::clamp(texture_y, 0, m_height) * m_width;
		if (t->getBytesPerPixel() == 2)
		{
			return ((m_y_scale / (256.0f * 256.0f - 1)) * ((uint16_t*)t->getData())[idx]);
		}
		else if(t->getBytesPerPixel() == 4)
		{
			return ((m_y_scale / 255.0f) * ((uint8_t*)t->getData())[idx * 4]);
		}
		else
		{
			ASSERT(false);
		}
		return 0;
	}


	bool getRayTriangleIntersection(const Vec3& local_origin, const Vec3& local_dir, const Vec3& p0, const Vec3& p1, const Vec3& p2, float& out)
	{
		Vec3 normal = crossProduct(p1 - p0, p2 - p0);
		float q = dotProduct(normal, local_dir);
		if (q == 0)
		{
			return false;
		}
		float d = -dotProduct(normal, p0);
		float t = -(dotProduct(normal, local_origin) + d) / q;
		if (t < 0)
		{
			return false;
		}
		Vec3 hit_point = local_origin + local_dir * t;

		Vec3 edge0 = p1 - p0;
		Vec3 VP0 = hit_point - p0;
		if (dotProduct(normal, crossProduct(edge0, VP0)) < 0)
		{
			return false;
		}

		Vec3 edge1 = p2 - p1;
		Vec3 VP1 = hit_point - p1;
		if (dotProduct(normal, crossProduct(edge1, VP1)) < 0)
		{
			return false;
		}

		Vec3 edge2 = p0 - p2;
		Vec3 VP2 = hit_point - p2;
		if (dotProduct(normal, crossProduct(edge2, VP2)) < 0)
		{
			return false;
		}

		out = t;
		return true;
	}

	
	RayCastModelHit Terrain::castRay(const Vec3& origin, const Vec3& dir)
	{
		RayCastModelHit hit;
		hit.m_is_hit = false;
		if (m_material && m_material->isReady())
		{
			Matrix mtx = m_entity.getMatrix();
			mtx.fastInverse();
			Vec3 rel_origin = mtx.multiplyPosition(origin);
			Vec3 rel_dir = mtx * dir;
			Vec3 start;
			Vec3 size(m_root->m_size * m_xz_scale, m_y_scale, m_root->m_size * m_xz_scale);
			if (Math::getRayAABBIntersection(rel_origin, rel_dir, m_root->m_min, size, start))
			{
				int hx = (int)(start.x / m_xz_scale);
				int hz = (int)(start.z / m_xz_scale);

				float next_x = ((hx + (rel_dir.x < 0 ? 0 : 1)) * m_xz_scale - rel_origin.x) / rel_dir.x;
				float next_z = ((hz + (rel_dir.z < 0 ? 0 : 1)) * m_xz_scale - rel_origin.z) / rel_dir.z;

				float delta_x = m_xz_scale / Math::abs(rel_dir.x);
				float delta_z = m_xz_scale / Math::abs(rel_dir.z);
				int step_x = (int)Math::signum(rel_dir.x);
				int step_z = (int)Math::signum(rel_dir.z);

				while (hx >= 0 && hz >= 0 && hx < m_width && hz < m_height)
				{
					float t;
					float x = hx * m_xz_scale;
					float z = hz * m_xz_scale;
					Vec3 p0(x, getHeight(x, z), z);
					Vec3 p1(x + m_xz_scale, getHeight(x + m_xz_scale, z), z);
					Vec3 p2(x + m_xz_scale, getHeight(x + m_xz_scale, z + m_xz_scale), z + m_xz_scale);
					Vec3 p3(x, getHeight(x, z + m_xz_scale), z + m_xz_scale);
					if (getRayTriangleIntersection(rel_origin, rel_dir, p0, p1, p2, t))
					{
						hit.m_is_hit = true;
						hit.m_origin = origin;
						hit.m_dir = dir;
						hit.m_t = t;
						return hit;
					}
					if (getRayTriangleIntersection(rel_origin, rel_dir, p0, p2, p3, t))
					{
						hit.m_is_hit = true;
						hit.m_origin = origin;
						hit.m_dir = dir;
						hit.m_t = t;
						return hit;
					}
					if (next_x < next_z)
					{
						next_x += delta_x;
						hx += step_x;
					}
					else
					{
						next_z += delta_z;
						hz += step_z;
					}
				}
			}
		}
		return hit;
	}


	static TerrainQuad* generateQuadTree(float size)
	{
		TerrainQuad* root = LUMIX_NEW(TerrainQuad);
		root->m_lod = 1;
		root->m_min.set(0, 0, 0);
		root->m_size = size;
		root->createChildren();
		return root;
	}


	static void generateSubgrid(Array<Sample>& samples, Array<int32_t>& indices, int& indices_offset, int start_x, int start_y)
	{
		for (int j = start_y; j < start_y + 8; ++j)
		{
			for (int i = start_x; i < start_x + 8; ++i)
			{
				int idx = 4 * (i + j * GRID_SIZE);
				samples[idx].pos.set((float)(i) / GRID_SIZE, 0, (float)(j) / GRID_SIZE);
				samples[idx + 1].pos.set((float)(i + 1) / GRID_SIZE, 0, (float)(j) / GRID_SIZE);
				samples[idx + 2].pos.set((float)(i + 1) / GRID_SIZE, 0, (float)(j + 1) / GRID_SIZE);
				samples[idx + 3].pos.set((float)(i) / GRID_SIZE, 0, (float)(j + 1) / GRID_SIZE);
				samples[idx].u = 0;
				samples[idx].v = 0;
				samples[idx + 1].u = 1;
				samples[idx + 1].v = 0;
				samples[idx + 2].u = 1;
				samples[idx + 2].v = 1;
				samples[idx + 3].u = 0;
				samples[idx + 3].v = 1;

				indices[indices_offset] = idx;
				indices[indices_offset + 1] = idx + 3;
				indices[indices_offset + 2] = idx + 2;
				indices[indices_offset + 3] = idx;
				indices[indices_offset + 4] = idx + 2;
				indices[indices_offset + 5] = idx + 1;
				indices_offset += 6;
			}
		}
	}

	void Terrain::generateGeometry()
	{
		LUMIX_DELETE(m_mesh);
		m_mesh = NULL;
		Array<Sample> points;
		points.resize(GRID_SIZE * GRID_SIZE * 4);
		Array<int32_t> indices;
		indices.resize(GRID_SIZE * GRID_SIZE * 6);
		int indices_offset = 0;
		generateSubgrid(points, indices, indices_offset, 0, 0);
		generateSubgrid(points, indices, indices_offset, 8, 0);
		generateSubgrid(points, indices, indices_offset, 0, 8);
		generateSubgrid(points, indices, indices_offset, 8, 8);

		VertexDef vertex_def;
		vertex_def.parse("pt", 2);
		m_geometry.copy((const uint8_t*)&points[0], sizeof(points[0]) * points.size(), indices, vertex_def);
		m_mesh = LUMIX_NEW(Mesh)(m_material, 0, indices.size(), "terrain");
	}

	void Terrain::onMaterialLoaded(Resource::State, Resource::State new_state)
	{
		PROFILE_FUNCTION();
		if (new_state == Resource::State::READY)
		{
			LUMIX_DELETE(m_root);
			m_heightmap = m_material->getTextureByUniform("hm_texture");
			m_splatmap = m_material->getTextureByUniform("splat_texture");
			m_width = m_heightmap->getWidth();
			m_height = m_heightmap->getHeight();
			m_root = generateQuadTree((float)m_width);
		}
	}


} // namespace Lumix