#include "CashGen.h"
#include "CGTerrainManager.h"
#include "CGJob.h"

ACGTerrainManager::ACGTerrainManager()
{
	PrimaryActorTick.bCanEverTick = true;
}

ACGTerrainManager::~ACGTerrainManager()
{

}



void ACGTerrainManager::BeginPlay()
{
	Super::BeginPlay();

	FString threadName = "TerrainWorkerThread";

	for (int i = 0; i < myTerrainConfig.NumberOfThreads; i++)
	{
		myGeometryJobQueues.Emplace();

		myWorkerThreads.Add(FRunnableThread::Create
		(new FCGTerrainGeneratorWorker(this, &myTerrainConfig, &myGeometryJobQueues[i]),
			*threadName,
			0, EThreadPriority::TPri_BelowNormal, FPlatformAffinity::GetNoAffinityMask()));
	}
}

void ACGTerrainManager::BeginDestroy()
{
	for (auto& thread : myWorkerThreads)
	{
		if (thread != nullptr)
		{
			thread->Kill();
		}
	}

	Super::BeginDestroy();
}

void ACGTerrainManager::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	myTimeSinceLastSweep += DeltaSeconds;


	// Check for pending jobs
	for (int i = 0; i < myTerrainConfig.NumberOfThreads; i++)
	{
		FCGJob pendingJob;
		if (myPendingJobQueue.Peek(pendingJob))
		{
			// If there's free data to allocate, dequeue and send to worker thread
			if (myFreeMeshData[pendingJob.LOD].Num() > 0)
			{
				myPendingJobQueue.Dequeue(pendingJob);
				GetFreeMeshData(pendingJob);
				myGeometryJobQueues[i].Enqueue(pendingJob);
			}
		}
	}

	// Now check for Update jobs
	for (uint8 i = 0; i < myTerrainConfig.MeshUpdatesPerFrame; i++)
	{
		FCGJob updateJob;
		if (myUpdateJobQueue.Dequeue(updateJob))
		{
			milliseconds startMs = duration_cast<milliseconds>(
				system_clock::now().time_since_epoch()
				);

			updateJob.Tile->UpdateMesh(updateJob.LOD, updateJob.IsInPlaceUpdate, updateJob.Vertices, updateJob.Triangles, updateJob.Normals, updateJob.UV0, updateJob.VertexColors, updateJob.Tangents);

			updateJob.Tile->SetActorHiddenInGame(false);
			int32 updateMS = (duration_cast<milliseconds>(
				system_clock::now().time_since_epoch()
				) - startMs).count();

#ifdef UE_BUILD_DEBUG
			if (updateJob.LOD == 0)
			{
				GEngine->AddOnScreenDebugMessage(0, 5.f, FColor::Red, TEXT("Heightmap gen " + FString::FromInt(updateJob.HeightmapGenerationDuration) + "ms"));
				GEngine->AddOnScreenDebugMessage(1, 5.f, FColor::Red, TEXT("Erosion gen " + FString::FromInt(updateJob.ErosionGenerationDuration) + "ms"));
				GEngine->AddOnScreenDebugMessage(2, 5.f, FColor::Red, TEXT("MeshUpdate " + FString::FromInt(updateMS) + "ms"));
			}
#endif
			ReleaseMeshData(updateJob.LOD, updateJob.Data);
			myQueuedTiles.Remove(updateJob.Tile);
		}
	}


	// New actor processing stuff here
	// Gonna be changed ALOT


	if (myTimeSinceLastSweep >= mySweepTime)
	{
		// Run through our tracked pawns
		for (APawn*& pawn : myTrackedPawns)
		{
			// Compare current location to previous
			FIntVector2 oldSector = myPawnLocationMap[pawn];
			FIntVector2 newSector = GetSector(pawn->GetActorLocation());
			if (oldSector != newSector)
			{
				// Take care of spawning new sectors if necessary
				HandlePlayerSectorChange(pawn, oldSector, newSector);
			}
			// Now run through the timestamp each required sector so it doesn't get released
			for (FIntVector2& sector : GetRelevantSectorsForActor(pawn))
			{
				if (myTileHandleMap.Contains(sector))
				{
					myTileHandleMap[sector].myLastRequiredTimestamp = FDateTime::Now();
				}
				else
				{
					FCGTileHandle tileHandle;
					tileHandle.myHandle = GetFreeTile();
					tileHandle.myHandle->SetupTile(sector, &myTerrainConfig, FVector(0.f));
					tileHandle.myHandle->RepositionAndHide(10);
					myTileHandleMap.Add(sector, tileHandle);

					FCGJob job;
					job.Tile = tileHandle.myHandle;
					job.LOD = 0;
					job.IsInPlaceUpdate = false;


					CreateTileRefreshJob(job);
				}

			}

		}

		myTimeSinceLastSweep = 0.0f;
	}



	for (auto& elem : myTileHandleMap)
	{
		// The tile hasn't been required for 5 seconds, free it
		if (elem.Value.myLastRequiredTimestamp + FTimespan(0, 0, 0, 5, 0) < FDateTime::Now())
		{
			FreeTile(elem.Value.myHandle);
			myTileHandleMap.Remove(elem.Key);
		}
		
	}




}

ACGTile* ACGTerrainManager::GetFreeTile()
{
	ACGTile* result = nullptr;

	if (myFreeTiles.Num())
	{
		result = myFreeTiles.Pop();
		//result->SetActorHiddenInGame(false);
	}
	
	
	if (!result)
	{
		//FCGTileHandle tileHandle;
		result = GetWorld()->SpawnActor<ACGTile>(ACGTile::StaticClass(), FVector(0.0f), FRotator(0.0f));
		//tileHandle.myStatus = ETileStatus::SPAWNED;
		//tileHandle.myLastRequiredTimestamp = FDateTime::Now();
	}

	return result;
}

void ACGTerrainManager::FreeTile(ACGTile* aTile)
{
	//aTile->SetActorHiddenInGame(true);
	myFreeTiles.Push(aTile);
}

void ACGTerrainManager::HandlePlayerSectorChange(const APawn* aPawn, const FIntVector2& anOldSector, const FIntVector2& aNewSector)
{
	GEngine->AddOnScreenDebugMessage(0, 5.f, FColor::Green, TEXT("Sector change "));
	myPawnLocationMap[aPawn] = aNewSector;
}

FIntVector2 ACGTerrainManager::GetSector(const FVector& aLocation)
{
	FIntVector2 sector;
	sector.X = static_cast<int32>(aLocation.X) / (myTerrainConfig.TileXUnits * myTerrainConfig.UnitSize);
	sector.Y = static_cast<int32>(aLocation.Y) / (myTerrainConfig.TileYUnits * myTerrainConfig.UnitSize);

	return sector;
}


TArray<FIntVector2> ACGTerrainManager::GetRelevantSectorsForActor(const APawn* aPawn)
{
	TArray<FIntVector2> result;

	FIntVector2 rootSector = GetSector(aPawn->GetActorLocation());

	const int range2 = myTerrainConfig.LODs[0].SectorDistance * myTerrainConfig.LODs[0].SectorDistance;

	for (int x = 0; x < myTerrainConfig.LODs[0].SectorDistance * 3; x++)
	{
		for (int y = 0; y < myTerrainConfig.LODs[0].SectorDistance * 3; y++)
		{
			FIntVector2 newSector = FIntVector2(rootSector.X - 5 + x, rootSector.Y - 5 + y);
			FIntVector2 diff = newSector - rootSector;
			if ((diff.X * diff.X + diff.Y * diff.Y) < range2)
			{
				result.Add(newSector);
			}
			
		}
	}

	return result;
}

void ACGTerrainManager::SetTerrainConfig(FCGTerrainConfig aTerrainConfig)
{
	myTerrainConfig = aTerrainConfig;

	AllocateAllMeshDataStructures();
}



void ACGTerrainManager::AddPawn(APawn* aPawn)
{
	myTrackedPawns.Add(aPawn);
	FIntVector2 pawnSector = GetSector(aPawn->GetActorLocation());
	myPawnLocationMap.Add(aPawn, pawnSector);
	for (FIntVector2& sector : GetRelevantSectorsForActor(aPawn))
	{
			FCGTileHandle tileHandle;
			tileHandle.myHandle = GetWorld()->SpawnActor<ACGTile>(ACGTile::StaticClass(),
				FVector((myTerrainConfig.TileXUnits * myTerrainConfig.UnitSize * sector.X) ,
				(myTerrainConfig.TileYUnits * myTerrainConfig.UnitSize * sector.Y), 0.0f), FRotator(0.0f));
			tileHandle.myStatus = ETileStatus::SPAWNED;
			tileHandle.myLastRequiredTimestamp = FDateTime::Now();

			myTileHandleMap.Add(sector, tileHandle);

			if (!myQueuedTiles.Contains(tileHandle.myHandle))
			{
				FCGJob job;
				job.Tile = tileHandle.myHandle;
				job.LOD = 0;
				job.IsInPlaceUpdate = true;

				tileHandle.myHandle->SetupTile(sector, &myTerrainConfig, FVector(0.f));
				tileHandle.myHandle->RepositionAndHide(10);

				CreateTileRefreshJob(job);
			}

		

		

	}
}


void ACGTerrainManager::CreateTileRefreshJob(FCGJob aJob)
{
	if (aJob.LOD != 10)
	{
		myPendingJobQueue.Enqueue(aJob);
		myQueuedTiles.Add(aJob.Tile);
	}

}

bool ACGTerrainManager::GetFreeMeshData(FCGJob& aJob)
{
	// No free mesh data
	if (myFreeMeshData[aJob.LOD].Num() < 1)
	{
		return false;
	}
	else
	{
		FCGMeshData* dataToUse;
		// Use the first free data set, there'll always be one, we checked!
		for (FCGMeshData* data : myFreeMeshData[aJob.LOD])
		{
			dataToUse = data;
			break;
		}
		// Add to the in use set
		myInUseMeshData[aJob.LOD].Add(dataToUse);
		// Remove from the Free set
		myFreeMeshData[aJob.LOD].Remove(dataToUse);

		aJob.Vertices = &dataToUse->Vertices;
		aJob.Triangles = &dataToUse->Triangles;
		aJob.Normals = &dataToUse->Normals;
		aJob.UV0 = &dataToUse->UV0;
		aJob.VertexColors = &dataToUse->VertexColors;
		aJob.Tangents = &dataToUse->Tangents;
		aJob.HeightMap = &dataToUse->HeightMap;
		aJob.Data = dataToUse;
		return true;
	}

	return false;
}

void ACGTerrainManager::ReleaseMeshData(uint8 aLOD, FCGMeshData* aDataToRelease)
{
	myInUseMeshData[aLOD].Remove(aDataToRelease);
	myFreeMeshData[aLOD].Add(aDataToRelease);
}

/** Allocates data structures and pointers for mesh data **/
void ACGTerrainManager::AllocateAllMeshDataStructures()
{
	for (uint8 lod = 0; lod < myTerrainConfig.LODs.Num(); ++lod)
	{
		myMeshData.Add(FCGLODMeshData());
		myFreeMeshData.Add(TSet<FCGMeshData*>());
		myInUseMeshData.Add(TSet<FCGMeshData*>());

		myMeshData[lod].Data.Reserve(myTerrainConfig.MeshDataPoolSize);

		for (int j = 0; j < myTerrainConfig.MeshDataPoolSize; ++j)
		{
			myMeshData[lod].Data.Add(FCGMeshData());
			AllocateDataStructuresForLOD(&myMeshData[lod].Data[j], &myTerrainConfig, lod);
		}
	}

	for (uint8 lod = 0; lod < myTerrainConfig.LODs.Num(); ++lod)
	{
		for (int j = 0; j < myTerrainConfig.MeshDataPoolSize; ++j)
		{
			myFreeMeshData[lod].Add(&myMeshData[lod].Data[j]);
		}
	}

}

/************************************************************************/
/*  Allocates all the data structures for a single LOD mesh data
/*		Includes setting up triangles etc.
/************************************************************************/
bool ACGTerrainManager::AllocateDataStructuresForLOD(FCGMeshData* aData, FCGTerrainConfig* aConfig, const uint8 aLOD)
{
	int32 numXVerts = aLOD == 0 ? aConfig->TileXUnits + 1 : (aConfig->TileXUnits / myTerrainConfig.LODs[aLOD].ResolutionDivisor) + 1;
	int32 numYVerts = aLOD == 0 ? aConfig->TileYUnits + 1 : (aConfig->TileYUnits / myTerrainConfig.LODs[aLOD].ResolutionDivisor) + 1;

	int32 numTotalVertices = numXVerts * numYVerts + (aConfig->TileXUnits * 2) + (aConfig->TileYUnits * 2) + 4;

	aData->Vertices.Reserve(numTotalVertices);
	aData->Normals.Reserve(numTotalVertices);
	aData->UV0.Reserve(numTotalVertices);
	aData->VertexColors.Reserve(numTotalVertices);
	aData->Tangents.Reserve(numTotalVertices);

	// Generate the per vertex data sets
	for (int32 i = 0; i < (numTotalVertices); ++i)
	{
		aData->Vertices.Emplace(0.0f);
		aData->Normals.Emplace(0.0f, 0.0f, 1.0f);
		aData->UV0.Emplace(0.0f, 0.0f);
		aData->VertexColors.Emplace(FColor::Black);
		aData->Tangents.Emplace(0.0f, 0.0f, 0.0f);
	}

	// Heightmap needs to be larger than the mesh
	// Using vectors here is a bit wasteful, but it does make normal/tangent or any other
	// Geometric calculations based on the heightmap a bit easier. Easy enough to change to floats

	aData->HeightMap.Reserve((numXVerts + 2) * (numYVerts + 2));
	for (int32 i = 0; i < (numXVerts + 2) * (numYVerts + 2); ++i)
	{
		aData->HeightMap.Emplace(0.0f);
	}

	// Triangle indexes
	int32 terrainTris = ((numXVerts - 1) * (numYVerts - 1) * 6);
	int32 skirtTris = (((numXVerts - 1) * 2) + ((numYVerts - 1) * 2)) * 6;
	int32 numTris = terrainTris + skirtTris;
	aData->Triangles.Reserve(numTris);
	for (int32 i = 0; i < numTris; ++i)
	{
		aData->Triangles.Add(i);
	}

	// Now calculate triangles and UVs
	int32 triCounter = 0;
	int32 thisX, thisY;
	int32 rowLength;

	rowLength = aLOD == 0 ? aConfig->TileXUnits + 1 : (aConfig->TileXUnits / myTerrainConfig.LODs[aLOD].ResolutionDivisor + 1);
	float maxUV = aLOD == 0 ? 1.0f : 1.0f / aLOD;

	int32 exX = aLOD == 0 ? aConfig->TileXUnits : (aConfig->TileXUnits / myTerrainConfig.LODs[aLOD].ResolutionDivisor);
	int32 exY = aLOD == 0 ? aConfig->TileYUnits : (aConfig->TileYUnits / myTerrainConfig.LODs[aLOD].ResolutionDivisor);

	for (int32 y = 0; y < exY; ++y)
	{
		for (int32 x = 0; x < exX; ++x)
		{

			thisX = x;
			thisY = y;
			//TR
			aData->Triangles[triCounter] = thisX + ((thisY + 1) * (rowLength));
			triCounter++;
			//BL
			aData->Triangles[triCounter] = (thisX + 1) + (thisY * (rowLength));
			triCounter++;
			//BR
			aData->Triangles[triCounter] = thisX + (thisY * (rowLength));
			triCounter++;

			//BL
			aData->Triangles[triCounter] = (thisX + 1) + (thisY * (rowLength));
			triCounter++;
			//TR
			aData->Triangles[triCounter] = thisX + ((thisY + 1) * (rowLength));
			triCounter++;
			// TL
			aData->Triangles[triCounter] = (thisX + 1) + ((thisY + 1) * (rowLength));
			triCounter++;

			//TR
			aData->UV0[thisX + ((thisY + 1) * (rowLength))] = FVector2D(thisX * maxUV, (thisY + 1.0f) * maxUV);
			//BR
			aData->UV0[thisX + (thisY * (rowLength))] = FVector2D(thisX * maxUV, thisY * maxUV);
			//BL
			aData->UV0[(thisX + 1) + (thisY * (rowLength))] = FVector2D((thisX + 1.0f) * maxUV, thisY * maxUV);
			//TL
			aData->UV0[(thisX + 1) + ((thisY + 1) * (rowLength))] = FVector2D((thisX + 1.0f)* maxUV, (thisY + 1.0f) * maxUV);

		}
	}

	return true;

}