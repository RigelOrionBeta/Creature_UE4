// UE4 Procedural Mesh Generation from the Epic Wiki (https://wiki.unrealengine.com/Procedural_Mesh_Generation)
//
// forked from "Engine/Plugins/Runtime/CustomMeshComponent/Source/CustomMeshComponent/Private/CustomMeshComponent.cpp"

#include "CustomPackProceduralMeshComponent.h"
#include "DynamicMeshBuilder.h"
#include <Materials/Material.h>
#include <Engine/CollisionProfile.h>
#include "Runtime/Launch/Resources/Version.h"
#include <Runtime/Core/Public/Async/ParallelFor.h>
#include "Engine/Engine.h"

static TAutoConsoleVariable<int32> CVarShowCreaturePackMeshes(
	TEXT("creature.ShowMeshes"),
	1,
	TEXT("Toggles a 'ShowFlag' for creature meshes.\n")
	TEXT("0: hidden\n")
	TEXT("1: rendered"),
	ECVF_RenderThreadSafe);


static FVertexBufferRHIRef AllocVertexBuffer(uint32 Stride, uint32 NumElements)
{
	FVertexBufferRHIRef VertexBufferRHI;
	uint32 SizeInBytes = NumElements * Stride;
	FRHIResourceCreateInfo CreateInfo;
	VertexBufferRHI = RHICreateVertexBuffer(SizeInBytes, BUF_Volatile | BUF_ShaderResource, CreateInfo);

	return VertexBufferRHI;
}

static void ReleaseVertexBuffer(FVertexBuffer& VertexBuffer)
{
}

/** Vertex Buffer */
class FProceduralMeshVertexBuffer : public FDynamicPrimitiveResource, public FRenderResource
{
public:
	FVertexBuffer PositionBuffer;
	FVertexBuffer TangentBuffer;
	FVertexBuffer TexCoordBuffer;
	FVertexBuffer ColorBuffer;

	FShaderResourceViewRHIRef TangentBufferSRV;
	FShaderResourceViewRHIRef TexCoordBufferSRV;
	FShaderResourceViewRHIRef ColorBufferSRV;
	FShaderResourceViewRHIRef PositionBufferSRV;

	mutable TArray<FDynamicMeshVertex> Vertices;

	FProceduralMeshVertexBuffer(uint32 InNumTexCoords = 1, uint32 InLightmapCoordinateIndex = 0, bool InUse16bitTexCoord = false) : NumTexCoords(InNumTexCoords), LightmapCoordinateIndex(InLightmapCoordinateIndex), Use16bitTexCoord(InUse16bitTexCoord)
	{
		check(NumTexCoords > 0 && NumTexCoords <= MAX_STATIC_TEXCOORDS);
		check(LightmapCoordinateIndex < NumTexCoords);
		buffersAllocated = false;
	}

	bool buffersAllocated;

	// FRenderResource interface.
	virtual void InitRHI() override
	{
		uint32 TextureStride = sizeof(FVector2D);
		EPixelFormat TextureFormat = PF_G32R32F;

		if (Use16bitTexCoord)
		{
			TextureStride = sizeof(FVector2DHalf);
			TextureFormat = PF_G16R16F;
		}

		if (!buffersAllocated)
		{
			PositionBuffer.VertexBufferRHI = AllocVertexBuffer(sizeof(FVector), Vertices.Num());
			TangentBuffer.VertexBufferRHI = AllocVertexBuffer(sizeof(FPackedNormal), 2 * Vertices.Num());
			TexCoordBuffer.VertexBufferRHI = AllocVertexBuffer(TextureStride, NumTexCoords * Vertices.Num());
			ColorBuffer.VertexBufferRHI = AllocVertexBuffer(sizeof(FColor), Vertices.Num());

			TangentBufferSRV = RHICreateShaderResourceView(TangentBuffer.VertexBufferRHI, 4, PF_R8G8B8A8);
			TexCoordBufferSRV = RHICreateShaderResourceView(TexCoordBuffer.VertexBufferRHI, TextureStride, TextureFormat);
			ColorBufferSRV = RHICreateShaderResourceView(ColorBuffer.VertexBufferRHI, 4, PF_R8G8B8A8);
			PositionBufferSRV = RHICreateShaderResourceView(PositionBuffer.VertexBufferRHI, sizeof(float), PF_R32_FLOAT);
			buffersAllocated = true;
		}

		void* TexCoordBufferData = RHILockVertexBuffer(TexCoordBuffer.VertexBufferRHI, 0, NumTexCoords * TextureStride * Vertices.Num(), RLM_WriteOnly);
		FVector2D* TexCoordBufferData32 = !Use16bitTexCoord ? static_cast<FVector2D*>(TexCoordBufferData) : nullptr;
		FVector2DHalf* TexCoordBufferData16 = Use16bitTexCoord ? static_cast<FVector2DHalf*>(TexCoordBufferData) : nullptr;

		// Copy the vertex data into the vertex buffers.
		FVector* PositionBufferData = static_cast<FVector*>(RHILockVertexBuffer(PositionBuffer.VertexBufferRHI, 0, sizeof(FVector) * Vertices.Num(), RLM_WriteOnly));
		FPackedNormal* TangentBufferData = static_cast<FPackedNormal*>(RHILockVertexBuffer(TangentBuffer.VertexBufferRHI, 0, 2 * sizeof(FPackedNormal) * Vertices.Num(), RLM_WriteOnly));
		FColor* ColorBufferData = static_cast<FColor*>(RHILockVertexBuffer(ColorBuffer.VertexBufferRHI, 0, sizeof(FColor) * Vertices.Num(), RLM_WriteOnly));

		for (int32 i = 0; i < Vertices.Num(); i++)
		{
			PositionBufferData[i] = Vertices[i].Position;
			TangentBufferData[2 * i + 0] = Vertices[i].TangentX;
			TangentBufferData[2 * i + 1] = Vertices[i].TangentZ;
			ColorBufferData[i] = Vertices[i].Color;

			for (uint32 j = 0; j < NumTexCoords; j++)
			{
				if (Use16bitTexCoord)
				{
					TexCoordBufferData16[NumTexCoords * i + j] = FVector2DHalf(Vertices[i].TextureCoordinate[j]);
				}
				else
				{
					TexCoordBufferData32[NumTexCoords * i + j] = Vertices[i].TextureCoordinate[j];
				}
			}
		}

		RHIUnlockVertexBuffer(PositionBuffer.VertexBufferRHI);
		RHIUnlockVertexBuffer(TangentBuffer.VertexBufferRHI);
		RHIUnlockVertexBuffer(TexCoordBuffer.VertexBufferRHI);
		RHIUnlockVertexBuffer(ColorBuffer.VertexBufferRHI);
	}

	void InitResource() override
	{
		FRenderResource::InitResource();
		PositionBuffer.InitResource();
		TangentBuffer.InitResource();
		TexCoordBuffer.InitResource();
		ColorBuffer.InitResource();
	}

	void ReleaseResource() override
	{
		FRenderResource::ReleaseResource();
		PositionBuffer.ReleaseResource();
		TangentBuffer.ReleaseResource();
		TexCoordBuffer.ReleaseResource();
		ColorBuffer.ReleaseResource();
	}

	virtual void ReleaseRHI() override
	{
		ReleaseVertexBuffer(PositionBuffer);
		ReleaseVertexBuffer(TangentBuffer);
		ReleaseVertexBuffer(TexCoordBuffer);
		ReleaseVertexBuffer(ColorBuffer);
		buffersAllocated = false;
	}

	// FDynamicPrimitiveResource interface.
	virtual void InitPrimitiveResource() override
	{
		InitResource();
	}

	virtual void ReleasePrimitiveResource() override
	{
		ReleaseResource();
		delete this;
	}

	const uint32 GetNumTexCoords() const
	{
		return NumTexCoords;
	}

	const uint32 GetLightmapCoordinateIndex() const
	{
		return LightmapCoordinateIndex;
	}

	const bool GetUse16bitTexCoords() const
	{
		return Use16bitTexCoord;
	}
private:
	const uint32 NumTexCoords;
	const uint32 LightmapCoordinateIndex;
	const bool Use16bitTexCoord;
};

/** Index Buffer */
class FProceduralMeshIndexBuffer : public FIndexBuffer
{
public:
	TArray<int32> Indices;

	virtual void InitRHI() override
	{
		FRHIResourceCreateInfo CreateInfo;
		IndexBufferRHI = RHICreateIndexBuffer(sizeof(int32), Indices.Num() * sizeof(int32), BUF_Dynamic, CreateInfo);
		UpdateRenderData();
	}

	void UpdateRenderData() const
	{
		// Copy the index data into the indices buffer
		void* Buffer = RHILockIndexBuffer(IndexBufferRHI, 0, Indices.Num() * sizeof(int32), RLM_WriteOnly);
		FMemory::Memcpy(Buffer, Indices.GetData(), Indices.Num() * sizeof(int32));
		RHIUnlockIndexBuffer(IndexBufferRHI);
	}
};

/** Vertex Factory */
class FProceduralMeshVertexFactory : public FDynamicPrimitiveResource, public FLocalVertexFactory
{
public:

	/** Initialization constructor. */
	FProceduralMeshVertexFactory(ERHIFeatureLevel::Type InFeatureLevel, const FProceduralMeshVertexBuffer* InVertexBuffer) : FLocalVertexFactory(InFeatureLevel, "FPooledDynamicMeshVertexFactory"), VertexBuffer(InVertexBuffer) {}

	void InitResource() override
	{
		FLocalVertexFactory* VertexFactory = this;
		const FProceduralMeshVertexBuffer* PooledVertexBuffer = VertexBuffer;
		ENQUEUE_RENDER_COMMAND(InitProceduralMeshVertexFactory)(
			[VertexFactory, PooledVertexBuffer](FRHICommandListImmediate& RHICmdList)
		{
			FDataType Data;
			Data.PositionComponent = FVertexStreamComponent(
				&PooledVertexBuffer->PositionBuffer,
				0,
				sizeof(FVector),
				VET_Float3
			);

			Data.NumTexCoords = PooledVertexBuffer->GetNumTexCoords();

			{
				Data.LightMapCoordinateIndex = PooledVertexBuffer->GetLightmapCoordinateIndex();
				Data.TangentsSRV = PooledVertexBuffer->TangentBufferSRV;
				Data.TextureCoordinatesSRV = PooledVertexBuffer->TexCoordBufferSRV;
				Data.ColorComponentsSRV = PooledVertexBuffer->ColorBufferSRV;
				Data.PositionComponentSRV = PooledVertexBuffer->PositionBufferSRV;
			}

			{
				EVertexElementType UVDoubleWideVertexElementType = VET_None;
				EVertexElementType UVVertexElementType = VET_None;
				uint32 UVSizeInBytes = 0;
				if (PooledVertexBuffer->GetUse16bitTexCoords())
				{
					UVSizeInBytes = sizeof(FVector2DHalf);
					UVDoubleWideVertexElementType = VET_Half4;
					UVVertexElementType = VET_Half2;
				}
				else
				{
					UVSizeInBytes = sizeof(FVector2D);
					UVDoubleWideVertexElementType = VET_Float4;
					UVVertexElementType = VET_Float2;
				}

				int32 UVIndex;
				uint32 UvStride = UVSizeInBytes * PooledVertexBuffer->GetNumTexCoords();
				for (UVIndex = 0; UVIndex < (int32)PooledVertexBuffer->GetNumTexCoords() - 1; UVIndex += 2)
				{
					Data.TextureCoordinates.Add
					(
						FVertexStreamComponent(
							&PooledVertexBuffer->TexCoordBuffer,
							UVSizeInBytes * UVIndex,
							UvStride,
							UVDoubleWideVertexElementType,
							EVertexStreamUsage::ManualFetch
						)
					);
				}

				// possible last UV channel if we have an odd number
				if (UVIndex < (int32)PooledVertexBuffer->GetNumTexCoords())
				{
					Data.TextureCoordinates.Add(FVertexStreamComponent(
						&PooledVertexBuffer->TexCoordBuffer,
						UVSizeInBytes * UVIndex,
						UvStride,
						UVVertexElementType,
						EVertexStreamUsage::ManualFetch
					));
				}

				Data.TangentBasisComponents[0] = FVertexStreamComponent(&PooledVertexBuffer->TangentBuffer, 0, 2 * sizeof(FPackedNormal), VET_PackedNormal, EVertexStreamUsage::ManualFetch);
				Data.TangentBasisComponents[1] = FVertexStreamComponent(&PooledVertexBuffer->TangentBuffer, sizeof(FPackedNormal), 2 * sizeof(FPackedNormal), VET_PackedNormal, EVertexStreamUsage::ManualFetch);
				Data.ColorComponent = FVertexStreamComponent(&PooledVertexBuffer->ColorBuffer, 0, sizeof(FColor), VET_Color, EVertexStreamUsage::ManualFetch);
			}
			VertexFactory->SetData(Data);


		});

		if (IsInRenderingThread())
		{
			FLocalVertexFactory::InitResource();
		}
	}

	// FDynamicPrimitiveResource interface.
	void InitPrimitiveResource() override
	{
		InitResource();
	}

	void ReleasePrimitiveResource() override
	{
		ReleaseResource();
		delete this;
	}

private:
	const FProceduralMeshVertexBuffer* VertexBuffer;
};

/** Mesh Render Packet**/
class FProceduralPackMeshRenderPacket
{
public:
	FProceduralPackMeshRenderPacket(FProceduralPackMeshTriData * data_in, ERHIFeatureLevel::Type InFeatureLevel) :
		VertexFactory(InFeatureLevel, &VertexBuffer)
	{
		indices = data_in->indices;
		points = data_in->points;
		uvs = data_in->uvs;
		point_num = data_in->point_num;
		indices_num = data_in->indices_num;
		real_indices_num = indices_num;
		region_alphas = data_in->region_alphas;
		update_lock = data_in->update_lock;
		should_release = false;

		// ensure the vertex data to be sent to the RHI is initialized
		CreateDirectVertexData();
	}

	virtual ~FProceduralPackMeshRenderPacket()
	{
		if (should_release) {
			VertexBuffer.ReleaseResource();
			IndexBuffer.ReleaseResource();
			VertexFactory.ReleaseResource();
		}
	}

	void setRealIndicesNum(int32 num_in)
	{
		real_indices_num = (num_in > 0) ? num_in : indices_num;
	}

	void InitForRender()
	{
		BeginInitResource(&VertexBuffer);
		BeginInitResource(&IndexBuffer);
		BeginInitResource(&VertexFactory);

		should_release = true;
	}

	TArray<FDynamicMeshVertex> VertexCache;

	void CreateDirectVertexData()
	{
		const int x_id = 0;
		const int y_id = 2;
		const int z_id = 1;

		FScopeLock scope_lock(update_lock.Get());

		if (VertexCache.Num() != point_num)
		{
			VertexCache.Reset(point_num);
			VertexCache.AddUninitialized(point_num);
		}

#ifdef CREATURE_MULTICORE
		ParallelFor(this->point_num, [&](int32 i) {
#else
		for (int32 i = 0; i < this->point_num; i++) {
#endif
			FDynamicMeshVertex& curVert = VertexCache[i];

			int pos_idx = i * 3;
			curVert.Position = FVector(this->points[pos_idx + x_id],
				this->points[pos_idx + y_id],
				this->points[pos_idx + z_id]);

			float set_alpha = (*this->region_alphas)[i];
			curVert.Color = FColor(set_alpha, set_alpha, set_alpha, set_alpha);

			int uv_idx = i * 2;
			for (int texCoord = 0; texCoord < MAX_STATIC_TEXCOORDS; texCoord++)
			{
				curVert.TextureCoordinate[texCoord].Set(this->uvs[uv_idx], this->uvs[uv_idx + 1]);
			}
#ifdef CREATURE_MULTICORE
		});
#else
		}
#endif

		// Set Tangents
#ifdef CREATURE_MULTICORE
		ParallelFor(indices_num / 3, [&](int32 ref_idx) {
			int32 cur_indice = ref_idx * 3;
#else
		for (int32 cur_indice = 0; cur_indice < indices_num; cur_indice += 3) {
#endif
			FDynamicMeshVertex & vert0 = VertexCache[(indices[cur_indice])];
			FDynamicMeshVertex & vert1 = VertexCache[(indices[cur_indice + 1])];
			FDynamicMeshVertex & vert2 = VertexCache[(indices[cur_indice + 2])];

			const FVector Edge01 = (vert1.Position - vert0.Position);
			const FVector Edge02 = (vert2.Position - vert0.Position);

			const FVector TangentX = Edge01.GetSafeNormal();
			const FVector TangentZ = (Edge02 ^ Edge01).GetSafeNormal();
			const FVector TangentY = (TangentX ^ TangentZ).GetSafeNormal();

			vert0.SetTangents(TangentX, TangentY, TangentZ);
			vert1.SetTangents(TangentX, TangentY, TangentZ);
			vert2.SetTangents(TangentX, TangentY, TangentZ);
#ifdef CREATURE_MULTICORE
		});
#else
		}
#endif

	}

	void UpdateDirectVertexData() const
	{
		int32 numReadyVertices = VertexCache.Num();
		check(numReadyVertices == point_num);

		VertexBuffer.Vertices = VertexCache;
		VertexBuffer.InitRHI();
	}

	void UpdateDirectIndexData() const
	{
		FScopeLock scope_lock(update_lock.Get());
		void* Buffer = RHILockIndexBuffer(IndexBuffer.IndexBufferRHI, 0, indices_num * sizeof(int32), RLM_WriteOnly);

		FMemory::Memcpy(Buffer, indices, indices_num * sizeof(int32));

		RHIUnlockIndexBuffer(IndexBuffer.IndexBufferRHI);
	}

	mutable FProceduralMeshVertexBuffer VertexBuffer;
	FProceduralMeshIndexBuffer IndexBuffer;
	FProceduralMeshVertexFactory VertexFactory;
	uint32 * indices;
	float * points;
	float * uvs;
	int32 point_num, indices_num, real_indices_num;
	TArray<uint8> * region_alphas;
	TSharedPtr<FCriticalSection, ESPMode::ThreadSafe> update_lock;
	bool should_release;
};

/** Scene proxy */

FCProceduralPackMeshSceneProxy::FCProceduralPackMeshSceneProxy(
	UCustomPackProceduralMeshComponent* Component,
	FProceduralPackMeshTriData * targetTrisIn,
	const FColor& startColorIn)
	: FPrimitiveSceneProxy(Component),
	MaterialRelevance(Component->GetMaterialRelevance(GetScene().GetFeatureLevel()))
{
	parentComponent = Component;
	needs_index_updating = false;
	needs_index_update_num = -1;
	active_render_packet_idx = INDEX_NONE;

	UpdateMaterial();

	// Add each triangle to the vertex/index buffer
	if (targetTrisIn)
	{
		AddRenderPacket(targetTrisIn, startColorIn, GetScene().GetFeatureLevel());
	}
}

SIZE_T FCProceduralPackMeshSceneProxy::GetTypeHash() const
{
	static size_t UniquePointer;
	return reinterpret_cast<size_t>(&UniquePointer);
}

FCProceduralPackMeshSceneProxy::~FCProceduralPackMeshSceneProxy()
{
}

FProceduralPackMeshRenderPacket *
FCProceduralPackMeshSceneProxy::GetActiveRenderPacket()
{
	if (!renderPackets.IsValidIndex(active_render_packet_idx))
	{
		return nullptr;
	}

	return &renderPackets[active_render_packet_idx];
}

bool FCProceduralPackMeshSceneProxy::GetDoesActiveRenderPacketHaveVertices() const
{
	return renderPackets.IsValidIndex(active_render_packet_idx) && renderPackets[active_render_packet_idx].VertexCache.Num() > 0;
}

void FCProceduralPackMeshSceneProxy::UpdateMaterial()
{
	// Grab material
	Material = parentComponent->GetMaterial(0);
	if (Material == NULL)
	{
		Material = UMaterial::GetDefaultMaterial(MD_Surface);
	}

	needs_material_updating = false;
}

void FCProceduralPackMeshSceneProxy::AddRenderPacket(FProceduralPackMeshTriData * targetTrisIn, const FColor& startColorIn, ERHIFeatureLevel::Type featureLevel)
{
	FScopeLock packetLock(&renderPacketsCS);

	//FProceduralMeshRenderPacket new_packet(featureLevel);
	//FProceduralMeshRenderPacket& cur_packet = renderPackets.Add_GetRef(new_packet);

	renderPackets.Add(new FProceduralPackMeshRenderPacket(targetTrisIn, featureLevel));
	auto packetPtr = &(renderPackets.Last());
	auto &cur_packet = *packetPtr;

	auto& IndexBuffer = cur_packet.IndexBuffer;
	auto& VertexBuffer = cur_packet.VertexBuffer;
	auto& VertexFactory = cur_packet.VertexFactory;

	IndexBuffer.Indices.SetNum(cur_packet.indices_num);
	VertexBuffer.Vertices.SetNum(cur_packet.point_num);

	// Set topology/indices
	for (int32 i = 0; i < cur_packet.indices_num; i++)
	{
		IndexBuffer.Indices[i] = cur_packet.indices[i];
	}

	const int x_id = 0;
	const int y_id = 2;
	const int z_id = 1;

	// Fill initial points
	for (int32 i = 0; i < cur_packet.point_num; i++)
	{
		FDynamicMeshVertex &Vert = VertexBuffer.Vertices[i];
		int pos_idx = i * 3;
		Vert.Position = FVector(cur_packet.points[pos_idx + x_id], cur_packet.points[pos_idx + y_id], cur_packet.points[pos_idx + z_id]);

		Vert.Color = startColorIn;
		Vert.SetTangents(FVector(1, 0, 0), FVector(0, 1, 0), FVector(0, 0, 1));

		int uv_idx = i * 2;
		for (int texCoord = 0; texCoord < MAX_STATIC_TEXCOORDS; texCoord++)
		{
			Vert.TextureCoordinate[texCoord].Set(cur_packet.uvs[uv_idx], cur_packet.uvs[uv_idx + 1]);
		}
	}

	// Set Initial Rest Tangents
	for (int cur_indice = 0; cur_indice < cur_packet.indices_num; cur_indice += 3)
	{
		FDynamicMeshVertex& vert0 = VertexBuffer.Vertices[cur_packet.indices[cur_indice]];
		FDynamicMeshVertex& vert1 = VertexBuffer.Vertices[cur_packet.indices[cur_indice + 1]];
		FDynamicMeshVertex& vert2 = VertexBuffer.Vertices[cur_packet.indices[cur_indice + 2]];

		const FVector Edge01 = (vert1.Position - vert0.Position);
		const FVector Edge02 = (vert2.Position - vert0.Position);

		const FVector TangentX = Edge01.GetSafeNormal();
		const FVector TangentZ = (Edge02 ^ Edge01).GetSafeNormal();
		const FVector TangentY = (TangentX ^ TangentZ).GetSafeNormal();

		vert0.SetTangents(TangentX, TangentY, TangentZ);
		vert1.SetTangents(TangentX, TangentY, TangentZ);
		vert2.SetTangents(TangentX, TangentY, TangentZ);
	}

	// Init vertex factory
	VertexFactory.InitResource();

	// Enqueue initialization of render resource
	cur_packet.InitForRender();

	if (active_render_packet_idx == INDEX_NONE)
	{
		active_render_packet_idx = 0;
	}
}

void FCProceduralPackMeshSceneProxy::ResetAllRenderPackets()
{
	FScopeLock packetLock(&renderPacketsCS);

	renderPackets.Reset();
	active_render_packet_idx = INDEX_NONE;
}

void FCProceduralPackMeshSceneProxy::SetActiveRenderPacketIdx(int idxIn)
{
	FScopeLock packetLock(&renderPacketsCS);
	active_render_packet_idx = idxIn;
}

void FCProceduralPackMeshSceneProxy::UpdateDynamicComponentData()
{
	if (active_render_packet_idx < 0)
	{
		return;
	}

	if (needs_material_updating)
	{
		UpdateMaterial();
	}

	FScopeLock packetLock(&renderPacketsCS);

	auto& cur_packet = renderPackets[active_render_packet_idx];
	cur_packet.CreateDirectVertexData();
}

void FCProceduralPackMeshSceneProxy::SetNeedsMaterialUpdate(bool flag_in)
{
	needs_material_updating = flag_in;
}

void FCProceduralPackMeshSceneProxy::SetNeedsIndexUpdate(bool flag_in, int32 index_new_num)
{
	needs_index_updating = flag_in;
	needs_index_update_num = index_new_num;
}

void FCProceduralPackMeshSceneProxy::SetDynamicData_RenderThread()
{
	FScopeLock packetLock(&renderPacketsCS);

	if (active_render_packet_idx < 0)
	{
		return;
	}

	auto& cur_packet = renderPackets[active_render_packet_idx];

	cur_packet.UpdateDirectVertexData();
	if (needs_index_updating)
	{
		cur_packet.setRealIndicesNum(needs_index_update_num);
		cur_packet.UpdateDirectIndexData();
		needs_index_updating = false;
		needs_index_update_num = -1;
	}
}

void FCProceduralPackMeshSceneProxy::GetDynamicMeshElements(const TArray<const FSceneView*>& Views,
	const FSceneViewFamily& ViewFamily,
	uint32 VisibilityMap,
	FMeshElementCollector& Collector) const
{
	int32 showFlag = CVarShowCreaturePackMeshes.GetValueOnAnyThread();
	if (showFlag == 0)
	{
		// creature mesh rendering is disabled
		return;
	}

	if (active_render_packet_idx < 0)
	{
		return;
	}

	FScopeLock packetLock(&renderPacketsCS);

	auto& cur_packet = renderPackets[active_render_packet_idx];
	auto& VertexBuffer = cur_packet.VertexBuffer;
	auto& IndexBuffer = cur_packet.IndexBuffer;
	auto& VertexFactory = cur_packet.VertexFactory;
	const FEngineShowFlags& EngineShowFlags = ViewFamily.EngineShowFlags;

	if (cur_packet.point_num <= 0)
	{
		return;
	}

	FMaterialRenderProxy* MaterialProxy = Material->GetRenderProxy();
	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
	{
		if (VisibilityMap & (1 << ViewIndex))
		{
			const FSceneView* View = Views[ViewIndex];
			// Draw the mesh.

			FMeshBatch& Mesh = Collector.AllocateMesh();
			FMeshBatchElement& BatchElement = Mesh.Elements[0];
			BatchElement.IndexBuffer = &IndexBuffer;
			Mesh.bWireframe = false;
			Mesh.VertexFactory = &VertexFactory;
			Mesh.MaterialRenderProxy = MaterialProxy;
			BatchElement.PrimitiveUniformBuffer = nullptr;
			BatchElement.FirstIndex = 0;
			BatchElement.NumPrimitives = cur_packet.real_indices_num / 3;
			BatchElement.MinVertexIndex = 0;
			BatchElement.MaxVertexIndex = VertexBuffer.Vertices.Num() - 1;
			Mesh.ReverseCulling = IsLocalToWorldDeterminantNegative();
			Mesh.Type = PT_TriangleList;
			Mesh.DepthPriorityGroup = SDPG_World;
			Mesh.bCanApplyViewModeOverrides = false;
			Collector.AddMesh(ViewIndex, Mesh);
		}

		RenderBounds(Collector.GetPDI(ViewIndex), EngineShowFlags, GetBounds(), !parentComponent || !parentComponent->GetOwner() || IsSelected());
	}
}

FPrimitiveViewRelevance FCProceduralPackMeshSceneProxy::GetViewRelevance(const FSceneView* View) const
{
	FPrimitiveViewRelevance Result;
	Result.bDrawRelevance = true;// IsShown(View);
	Result.bShadowRelevance = IsShadowCast(View);
	Result.bDynamicRelevance = true;
	Result.bRenderInMainPass = ShouldRenderInMainPass();
	Result.bUsesLightingChannels = GetLightingChannelMask() != GetDefaultLightingChannelMask();
	Result.bRenderCustomDepth = ShouldRenderCustomDepth();

	MaterialRelevance.SetPrimitiveViewRelevance(Result);

	return Result;
}

bool FCProceduralPackMeshSceneProxy::CanBeOccluded() const
{
	return !MaterialRelevance.bDisableDepthTest;
}

uint32 FCProceduralPackMeshSceneProxy::GetMemoryFootprint(void) const
{
	return(sizeof(*this) + GetAllocatedSize());
}

uint32 FCProceduralPackMeshSceneProxy::GetAllocatedSize(void) const
{
	return(FPrimitiveSceneProxy::GetAllocatedSize());
}



//////////////////////////////////////////////////////////////////////////

UCustomPackProceduralMeshComponent::UCustomPackProceduralMeshComponent(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryComponentTick.bCanEverTick = false;
	bounds_scale = 1.0f;
	bounds_offset = FVector(0, 0, 0);
	render_proxy_ready = false;
	calc_local_vec_min = FVector(FLT_MIN, FLT_MIN, FLT_MIN);
	calc_local_vec_max = FVector(FLT_MAX, FLT_MAX, FLT_MAX);
	bWantsInitializeComponent = true;

	//	SetCollisionProfileName(UCollisionProfile::BlockAllDynamic_ProfileName);
	SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
}

bool UCustomPackProceduralMeshComponent::SetProceduralMeshTriData(const FProceduralPackMeshTriData& TriData)
{
	defaultTriData = TriData;

	//UpdateCollision();

	// Need to recreate scene proxy to send it over
	MarkRenderStateDirty();

	return true;
}

void UCustomPackProceduralMeshComponent::SendRenderDynamicData_Concurrent()
{
	FCProceduralPackMeshSceneProxy *proxy = GetLocalRenderProxy();
	if (proxy)
	{
		// Enqueue command to send to render thread
		FCProceduralPackMeshSceneProxy* sceneProxy = proxy;
		ENQUEUE_RENDER_COMMAND(FSendCreatureDynamicData)(
			[sceneProxy](FRHICommandListImmediate& RHICmdList)
		{
			sceneProxy->SetDynamicData_RenderThread();
		});
	}
}

void UCustomPackProceduralMeshComponent::RecreateRenderProxy(bool flag_in)
{
	recreate_render_proxy = flag_in;
}

void UCustomPackProceduralMeshComponent::ForceAnUpdate(int render_packet_idx, bool markDirty /*= true*/)
{
	FScopeLock cur_lock(&local_lock);

	// Need to recreate scene proxy to send it over
	if (recreate_render_proxy)
	{
		if (markDirty)
		{
			MarkRenderStateDirty();
			recreate_render_proxy = false;
		}
		return;
	}

	FCProceduralPackMeshSceneProxy *localRenderProxy = GetLocalRenderProxy();
	if (render_proxy_ready && localRenderProxy)
	{
		if (render_packet_idx >= 0)
		{
			localRenderProxy->SetActiveRenderPacketIdx(render_packet_idx);
		}

		localRenderProxy->UpdateDynamicComponentData();
		ProcessCalcBounds(localRenderProxy);

		if (markDirty)
		{
			MarkRenderTransformDirty();

			MarkRenderDynamicDataDirty();
		}
	}
}

void
UCustomPackProceduralMeshComponent::SetTagString(FString tag_in)
{
	tagStr = tag_in;
}

FPrimitiveSceneProxy* UCustomPackProceduralMeshComponent::CreateSceneProxy()
{
	FScopeLock cur_lock(&local_lock);

	FCProceduralPackMeshSceneProxy* Proxy = NULL;
	// Only if have enough triangles
	if (defaultTriData.point_num > 0)
	{
		auto not_editor_mode = ((GetWorld()->WorldType != EWorldType::Type::Editor) &&
			(GetWorld()->WorldType != EWorldType::Type::EditorPreview));
		FColor start_color = not_editor_mode ? FColor(0, 0, 0, 0) : FColor::White;
		Proxy = new FCProceduralPackMeshSceneProxy(this, &defaultTriData, start_color);

		SendRenderDynamicData_Concurrent();

		render_proxy_ready = true;

		ProcessCalcBounds(Proxy);
	}

	return Proxy;
}

int32 UCustomPackProceduralMeshComponent::GetNumMaterials() const
{
	return 1;
}

void UCustomPackProceduralMeshComponent::ProcessCalcBounds(FCProceduralPackMeshSceneProxy *localRenderProxy)
{
	FProceduralPackMeshRenderPacket * cur_packet = nullptr;
	bool can_calc = false;
	if (render_proxy_ready && localRenderProxy)
	{
		cur_packet = localRenderProxy->GetActiveRenderPacket();
		if (cur_packet)
		{
			can_calc = (cur_packet->point_num > 0);
		}
	}

	const float bounds_max_scalar = 100000.0f;
	calc_local_vec_min = FVector(-bounds_max_scalar, -bounds_max_scalar, -bounds_max_scalar);
	calc_local_vec_max = FVector(bounds_max_scalar, bounds_max_scalar, bounds_max_scalar);

	// Only if have enough triangles
	if (can_calc)
	{
		const int x_id = 0;
		const int y_id = 2;
		const int z_id = 1;

		auto cur_pts = cur_packet->points;

		// Minimum Vector: It's set to the first vertex's position initially (NULL == FVector::ZeroVector might be required and a known vertex vector has intrinsically valid values)
		FVector vecMin = FVector(cur_pts[x_id], cur_pts[y_id], cur_pts[z_id]);
		if ((vecMin.X == FLT_MIN) || (vecMin.Y == FLT_MIN) || (vecMin.Z == FLT_MIN)
			|| (vecMin.X == FLT_MAX) || (vecMin.Y == FLT_MAX) || (vecMin.Z == FLT_MAX))
		{
			vecMin.Set(0, 0, 0);
		}

		// Maximum Vector: It's set to the first vertex's position initially (NULL == FVector::ZeroVector might be required and a known vertex vector has intrinsically valid values)
		FVector vecMax = vecMin;

		// Get maximum and minimum X, Y and Z positions of vectors
		FVector vecMidPt(0, 0, 0);
		for (int32 i = 0; i < cur_packet->point_num; i++)
		{
			int32 ptIdx = i * 3;
			auto posX = cur_pts[ptIdx + x_id];
			auto posY = cur_pts[ptIdx + y_id];
			auto posZ = cur_pts[ptIdx + z_id];

			bool not_flt_min = (posX != FLT_MIN) && (posY != FLT_MIN) && (posZ != FLT_MIN);
			bool not_flt_max = (posX != FLT_MAX) && (posY != FLT_MAX) && (posZ != FLT_MAX);

			if (not_flt_min && not_flt_max) {
				vecMin.X = (vecMin.X > posX) ? posX : vecMin.X;

				vecMin.Y = (vecMin.Y > posY) ? posY : vecMin.Y;

				vecMin.Z = (vecMin.Z > posZ) ? posZ : vecMin.Z;

				vecMax.X = (vecMax.X < posX) ? posX : vecMax.X;

				vecMax.Y = (vecMax.Y < posY) ? posY : vecMax.Y;

				vecMax.Z = (vecMax.Z < posZ) ? posZ : vecMax.Z;
			}
		}

		const float lscale = bounds_scale;
		FVector lScaleVec(lscale, lscale, lscale);

		vecMidPt = (vecMax + vecMin) * 0.5f;
		vecMax = (vecMax - vecMidPt) * lScaleVec + vecMidPt;
		vecMin = (vecMin - vecMidPt) * lScaleVec + vecMidPt;

		if ((vecMin.X <= -bounds_max_scalar) || (vecMin.Y <= -bounds_max_scalar) || (vecMin.Z <= -bounds_max_scalar)
			|| (vecMin.X >= bounds_max_scalar) || (vecMin.Y >= bounds_max_scalar) || (vecMin.Z >= bounds_max_scalar) ||
			(vecMax.X <= -bounds_max_scalar) || (vecMax.Y <= -bounds_max_scalar) || (vecMax.Z <= -bounds_max_scalar)
			|| (vecMax.X >= bounds_max_scalar) || (vecMax.Y >= bounds_max_scalar) || (vecMax.Z >= bounds_max_scalar))
		{
			vecMin.Set(-bounds_max_scalar, -bounds_max_scalar, -bounds_max_scalar);
			vecMax.Set(bounds_max_scalar, bounds_max_scalar, bounds_max_scalar);
		}

		calc_local_vec_min = vecMin;
		calc_local_vec_max = vecMax;

		debugSphere = FBoxSphereBounds(FBox(calc_local_vec_min, calc_local_vec_max)).GetSphere();
	}
}

FBoxSphereBounds UCustomPackProceduralMeshComponent::CalcBounds(const FTransform & LocalToWorld) const
{
	FBoxSphereBounds ret_bounds = FBoxSphereBounds(FBox(calc_local_vec_min, calc_local_vec_max));

	if (ret_bounds.ContainsNaN())
	{
		ret_bounds = FBoxSphereBounds(FBox(FVector(-100, -100, -100),
			FVector(100, 100, 100)));
	}

	// transform the bounds by the given transform
	ret_bounds = ret_bounds.TransformBy(LocalToWorld);

	return ret_bounds;
}

void UCustomPackProceduralMeshComponent::SetBoundsScale(float value_in)
{
	bounds_scale = value_in;
}

void UCustomPackProceduralMeshComponent::SetBoundsOffset(const FVector& offset_in)
{
	bounds_offset = offset_in;
}

FSphere
UCustomPackProceduralMeshComponent::GetDebugBoundsSphere() const
{
	return debugSphere.TransformBy(GetComponentTransform());
}

/*
bool UCustomPackProceduralMeshComponent::GetPhysicsTriMeshData(struct FTriMeshCollisionData* CollisionData, bool InUseAllTriData)
{
FTriIndices Triangle;

for(int32 i = 0; i<ProceduralMeshTris.Num(); i++)
{
const FProceduralMeshTriangle& tri = ProceduralMeshTris[i];

Triangle.v0 = CollisionData->Vertices.Add(tri.Vertex0.Position);
Triangle.v1 = CollisionData->Vertices.Add(tri.Vertex1.Position);
Triangle.v2 = CollisionData->Vertices.Add(tri.Vertex2.Position);

CollisionData->Indices.Add(Triangle);
CollisionData->MaterialIndices.Add(i);
}

CollisionData->bFlipNormals = true;

return true;
}

bool UCustomPackProceduralMeshComponent::ContainsPhysicsTriMeshData(bool InUseAllTriData) const
{
return (ProceduralMeshTris.Num() > 0);
}
*/

void UCustomPackProceduralMeshComponent::UpdateBodySetup()
{
	if (ModelBodySetup == NULL)
	{
		/*
		ModelBodySetup = ConstructObject<UBodySetup>(UBodySetup::StaticClass(), this);
		ModelBodySetup->CollisionTraceFlag = CTF_UseComplexAsSimple;
		ModelBodySetup->bMeshCollideAll = true;
		*/
	}
}

void UCustomPackProceduralMeshComponent::UpdateCollision()
{
	/*
	if(bPhysicsStateCreated)
	{
	DestroyPhysicsState();
	UpdateBodySetup();
	CreatePhysicsState();

	// Works in Packaged build only since UE4.5:
	ModelBodySetup->InvalidatePhysicsData();
	ModelBodySetup->CreatePhysicsMeshes();
	}
	*/
}

UBodySetup* UCustomPackProceduralMeshComponent::GetBodySetup()
{
	UpdateBodySetup();
	return ModelBodySetup;
}

void UCustomPackProceduralMeshComponent::InitializeComponent()
{
	UMeshComponent::InitializeComponent();
	render_proxy_ready = false;
	MarkRenderStateDirty();
}
