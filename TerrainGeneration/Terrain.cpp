#include "Terrain.h"
#include "Logger.h"
#include "Scene.h"

#include <chrono>


#include "Shader.h"
#include "Texture.h"
#include "Material.h"
#include "Transform.h"

Texture* testTexture0;
Texture* testTexture1;
Texture* testTexture2;
Texture* testTexture3;
Shader* testShader;
Material* testMaterial;



Terrain::Terrain(Scene* scene) :
	m_parent(scene)
{
	m_workerThread = new std::thread(&Terrain::RunWorker, this);
	m_activeChunks.reserve(sizeof(ivec2) * m_poolSize * 2);

	// Setup chunk pool
	for (uint32 i = 0; i < m_poolSize; ++i)
		m_chunkPool.emplace(new Chunk(this));
	LOG("Built chunk pool of size %i", m_poolSize);



	testShader = new Shader;
	testShader->LoadVertexShaderFromMemory(R"(
		#version 420 core

		// Transform matrix
		uniform mat4 ObjectToWorld;

		// View matrix
		uniform mat4 WorldToView;

		// Perspective matrix
		uniform mat4 ViewToClip;

		layout (location = 0) in vec3 inPos;
		layout (location = 1) in vec3 inNormal;
		layout (location = 2) in vec4 inColour;

		out vec3 passToCamera;
		out vec3 passPos;
		out vec3 passNormal;
		out vec4 passColour;

		void main()
		{
			vec4 worldLocation = ObjectToWorld * vec4(inPos, 1);
			vec3 cameraLocation = vec3(ViewToClip[0][3], ViewToClip[1][3], ViewToClip[2][3]);
			passToCamera = worldLocation.xyz - cameraLocation;

			gl_Position = ViewToClip * WorldToView * worldLocation;
			passPos = worldLocation.xyz;
			passNormal = inNormal;
			passColour = inColour;
		}
	)");
	testShader->LoadFragmentShaderFromMemory(R"(
		#version 420 core

				
		in vec3 passToCamera;
		in vec3 passPos;
		in vec3 passNormal;
		in vec4 passColour;

		out vec4 outColour;

		layout(binding = 0) uniform sampler2D texChannel0;
		layout(binding = 1) uniform sampler2D texChannel1;
		layout(binding = 2) uniform sampler2D texChannel2;
		layout(binding = 3) uniform sampler2D texChannel3;


		/// Get the texture colour for this normal
		vec4 GetTextureColour(sampler2D tex, vec3 anorm)
		{
			vec4 value;

			// Perform tri-planar projection
			if(anorm.x != 0)
				value = anorm.x * texture(tex, passPos.yz);
			if(anorm.y != 0)
				value = value + anorm.y * texture(tex, passPos.xz);
			if(anorm.z != 0)
				value = value + anorm.z * texture(tex, passPos.xy);

			return value;
		}

		/// Calculate colour
		vec4 GetColour()
		{
			vec3 anorm = normalize(vec3(abs(passNormal.x), abs(passNormal.y), abs(passNormal.z)));
			vec4 value;

			if(passColour.r != 0)
				value = passColour.r * GetTextureColour(texChannel0, anorm);
			if(passColour.g != 0)
				value = value + passColour.g * GetTextureColour(texChannel1, anorm);
			if(passColour.b != 0)
				value = value + passColour.b * GetTextureColour(texChannel2, anorm);
			if(passColour.a != 0)
				value = value + passColour.a * GetTextureColour(texChannel3, anorm);

			return value;
		}

		void main()
		{
			vec3 lightDirection = normalize(vec3(1, -1, 1));
			vec3 normal = normalize(passNormal);
			float diffuse = max(dot(-lightDirection, normal), 0.2);

			// Test texture based on normal/face
			//outColour.rgb = texture(texChannel1, GetUVs()).rgb;
			outColour = GetColour();
		}
	)");
	testShader->LinkShader();

	testMaterial = new Material(testShader);

	testTexture0 = new Texture;
	testTexture0->LoadFromFile("Resources\\grass.png");
	testTexture1 = new Texture;
	testTexture1->LoadFromFile("Resources\\dirt.jpg");
	testTexture2 = new Texture;
	testTexture2->LoadFromFile("Resources\\sand.png");
	testTexture3 = new Texture;
	testTexture3->LoadFromFile("Resources\\stone.jpg");
}

Terrain::~Terrain()
{
	// Clean up worker thread
	if (m_workerThread != nullptr)
	{
		LOG("Waiting for worker to finish..");
		bWorkerRunning = false;
		m_workerThread->join();
		delete m_workerThread;
	}

	// Clean up chunk pool
	for (auto it : m_activeChunks)
		delete it.second;
	while (m_chunkPool.size() != 0)
	{
		Chunk* chunk = m_chunkPool.front();
		m_chunkPool.pop();
		delete chunk;
	}



	delete testMaterial;
	delete testShader;
	delete testTexture0;
	delete testTexture1;
	delete testTexture2;
	delete testTexture3;
}

void Terrain::RunWorker() 
{
	bWorkerRunning = true;
	LOG("Launching Terrain worker thread");

	while (bWorkerRunning)
	{
		try
		{
			// Try to execute all jobs, if there are any
			while (m_activeJobQueue.size() != 0)
			{
				IChunkJob* job = m_activeJobQueue.front();

				// Only execute job, if it hasn't been aborted
				if (!job->IsAborted())
					job->Execute();

				// Job may have aborted during execution
				if (!job->IsAborted())
					m_completedJobQueue.emplace(job);
				else
					delete job;

				m_activeJobQueue.pop();
			}

			// Update at 60 ticks a second
			std::this_thread::sleep_for(std::chrono::milliseconds(1000 / 60));
		}
		catch (std::exception e)
		{
			LOG_ERROR("Exception caught in worker thread '%s'", e.what());
		}
	}

	LOG("Terrain worker thread closed");
	bWorkerRunning = false;
}


bool Terrain::TryGetNewChunk(Chunk*& outChunk, const ivec2& coord)
{
	if (m_chunkPool.size() == 0)
		return false;

	// Take out of chunk pool
	outChunk = m_chunkPool.front();
	m_chunkPool.pop();

	outChunk->Alloc(coord);
	return true;
}
void Terrain::FreeChunk(Chunk* chunk) 
{
	// Add back into chunk pool
	m_chunkPool.push(chunk);
	chunk->Dealloc();
}


void Terrain::UpdateScene(Window& window, const float& deltaTime) 
{
	vec3 loadCentre = m_parent->GetCamera().GetLocation();
	ivec2 centre = GetChunkCoords(std::round(loadCentre.x), std::round(loadCentre.y), std::round(loadCentre.z));


	const int32 loadRange = 9;
	const int32 unloadRadius = 8; // Slightly larger to basically cache chunks

	// Unload any chunks which are outside of the area
	for (auto it = m_activeChunks.begin(); it != m_activeChunks.end();)
	{
		const ivec2& coords = it->first;
		if (std::abs(coords.x - centre.x) > unloadRadius || std::abs(coords.y - centre.y) > unloadRadius)
		{
			// TODO - Cleanup any active jobs
			FreeChunk(it->second);
			m_activeChunks.erase(it++);
		}
		else
			++it;
	}

	// Lambda for making sure a chunk is loaded
	auto ensureChunkLoads = 
	[this, centre](const uint32& x, const uint32& y)
	{
		ivec2 coords(centre.x + x, centre.y + y);
		if (m_activeChunks.find(coords) == m_activeChunks.end())
		{
			Chunk* chunk;
			if (TryGetNewChunk(chunk, coords))
				m_activeChunks[coords] = chunk;
			else
				LOG_WARNING("Cannot load new chunk (%i,%i) as pool is empty", coords.x, coords.y);
		}
	};

	// Load chunks in load radius (Build outwards from centre in a sprial)
	// https://stackoverflow.com/questions/398299/looping-in-a-spiral
	{
		int32 x = 0;
		int32 y = 0;
		int32 dx = 0;
		int32 dy = -1;

		const uint32 max = loadRange*loadRange;

		for (int32 i = 0; i < max; ++i)
		{
			if ((-loadRange / 2 < x) && (x <= loadRange / 2) && (-loadRange / 2 < y) && (y <= loadRange / 2))
				ensureChunkLoads(x, y);

			if ((x == y) || ((x < 0) && (x == -y)) || ((x > 0) && (x == 1 - y)))
			{
				int32 temp = dx;
				dx = -dy;
				dy = temp;
			}
			x += dx;
			y += dy;
		}
	}


	// Try to fetch any jobs
	for (auto it : m_activeChunks)
	{
		if (it.second->HasQueuedJob())
			m_activeJobQueue.emplace(it.second->GetQueuedJob());
	}

	// Complete any jobs that need it
	while (m_completedJobQueue.size() != 0)
	{
		IChunkJob* job = m_completedJobQueue.front();

		// Only complete if not aborted
		if (!job->IsAborted())
		{
			job->OnComplete();
			job->GetOwningChunk().OnJobCompletion(job);
		}

		m_completedJobQueue.pop();
		delete job;
	}
}

void Terrain::RenderTerrain(Window& window, const float& deltaTime) 
{
	testMaterial->Bind(window, *m_parent);


	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, testTexture0->GetID());
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, testTexture1->GetID());
	glActiveTexture(GL_TEXTURE2);
	glBindTexture(GL_TEXTURE_2D, testTexture2->GetID());
	glActiveTexture(GL_TEXTURE3);
	glBindTexture(GL_TEXTURE_2D, testTexture3->GetID());

	for (auto it : m_activeChunks)
	{
		if (it.second->IsTerrainMeshBuilt())
		{
			testMaterial->PrepareMesh(*it.second->GetTerrainMesh());
			testMaterial->RenderInstance(Transform());
		}
	}
}

Voxel::Type Terrain::Get(const int32& x, const int32& y, const int32& z) const 
{
	ivec2 coords = GetChunkCoords(x, y, z);

	auto it = m_activeChunks.find(coords);
	if (it == m_activeChunks.end())
		return Voxel::Type::Air;
	else
		return it->second->Get(x - coords.x * CHUNK_SIZE, y, z - coords.y * CHUNK_SIZE);
}

void Terrain::NotifyChunkGeneration(const ivec2& coords) 
{
	// Notify adjacent chunks of the change
	for (int32 x = -1; x <= 1; ++x)
		for (int32 y = -1; y <= 1; ++y)
		{
			if (x == 0 && y == 0)
				continue;

			auto it = m_activeChunks.find(ivec2(coords.x + x, coords.y + y));
			if (it != m_activeChunks.end())
				it->second->OnAdjacentChunkGenerate();
		}
}