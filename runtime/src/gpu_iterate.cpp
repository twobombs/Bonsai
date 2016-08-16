#undef NDEBUG
#include "octree.h"
#include  "postProcessModules.h"

#include <iostream>
#include <algorithm>

using namespace std;

static double de_max  = 0;
static double dde_max = 0;  


cudaEvent_t startLocalGrav;
cudaEvent_t startRemoteGrav;
cudaEvent_t endLocalGrav;
cudaEvent_t endRemoteGrav;

float runningLETTimeSum, lastTotal, lastLocal;


void octree::makeLET()
{
   //LET code test
  double t00 = get_time();

  //Start copies, while grpTree info is exchanged
  localTree.boxSizeInfo.d2h  (  localTree.n_nodes, false, LETDataToHostStream->s());
  localTree.boxCenterInfo.d2h(  localTree.n_nodes, false, LETDataToHostStream->s());
  localTree.multipole.d2h    (3*localTree.n_nodes, false, LETDataToHostStream->s());
  localTree.boxSizeInfo.waitForCopyEvent();
  localTree.boxCenterInfo.waitForCopyEvent();
  
  double t10 = get_time();
  //Exchange domain grpTrees, while memory copies take place
  this->sendCurrentInfoGrpTree();

  double t20 = get_time();


  localTree.multipole.waitForCopyEvent();
  double t40 = get_time();
  LOGF(stderr,"MakeLET Preparing data-copy: %lg  sendGroups: %lg Total: %lg \n",
               t10-t00, t20-t10, t40-t00);

  std::vector<real4> topLevelsBuffer;
  std::vector<uint2> treeSizeAndOffset;
  int copyTreeUpToLevel = 0;
  //Start LET kernels
  essential_tree_exchangeV2(localTree,
                            remoteTree,
                            topLevelsBuffer,
                            treeSizeAndOffset,
                            copyTreeUpToLevel);

  letRunning = false;
}



// returns true if this iteration is the last (t_current >= t_end), false otherwise
bool octree::iterate_once(IterationData &idata) {
    double t1 = 0;

    //if(t_current < 1) //Clear startup timings
    //if(0)
    if(iter < 32)
    {
      idata.totalGPUGravTimeLocal = 0;
      idata.totalGPUGravTimeLET   = 0;
      idata.totalLETCommTime      = 0;
      idata.totalBuildTime        = 0;
      idata.totalDomTime          = 0;
      idata.lastWaitTime          = 0;
      idata.startTime             = get_time();
      idata.totalGravTime         = 0;
      idata.totalDomUp            = 0;
      idata.totalDomEx            = 0;
      idata.totalDomWait          = 0;
      idata.totalPredCor          = 0;
    }


    LOG("At the start of iterate:\n");
    
    bool forceTreeRebuild = false;
    bool needDomainUpdate = true;

    double tTempTime = get_time();

    //predict local tree
    devContext.startTiming(execStream->s());
    predict(this->localTree);
    devContext.stopTiming("Predict", 9, execStream->s());

    idata.totalPredCor += get_time() - tTempTime;

    if(nProcs > 1)
    {
      //if(1) //Always update domain boundaries/particles
      if((iter % rebuild_tree_rate) == 0)
      {
        double domUp =0, domEx = 0;
        double tZ = get_time();
        devContext.startTiming(execStream->s());
        parallelDataSummary(localTree, lastTotal, lastLocal, domUp, domEx, false);
        devContext.stopTiming("UpdateDomain", 6, execStream->s());
        double tZZ = get_time();
        idata.lastDomTime   = tZZ-tZ;
        idata.totalDomTime += idata.lastDomTime;

        idata.totalDomUp += domUp;
        idata.totalDomEx += domEx;

        devContext.startTiming(execStream->s());
        mpiSync();
        devContext.stopTiming("DomainUnbalance", 12, execStream->s());

        idata.totalDomWait += get_time()-tZZ;

        needDomainUpdate    = false; //We did a boundary sync in the parallel decomposition part
        needDomainUpdate    = true; //TODO if I set it to false results degrade. Check why, for now just updte
      }
    }

    if (useDirectGravity)
    {
      devContext.startTiming(gravStream->s());
      direct_gravity(this->localTree);
      devContext.stopTiming("Direct_gravity", 4);
    }
    else
    {
      //Build the tree using the predicted positions
      // bool rebuild_tree = Nact_since_last_tree_rebuild > 4*this->localTree.n;   
      bool rebuild_tree = true;

      rebuild_tree = ((iter % rebuild_tree_rate) == 0);
      if(rebuild_tree)
      {
        //Rebuild the tree
        t1 = get_time();
        this->sort_bodies(this->localTree, needDomainUpdate);
        this->build(this->localTree);
        LOGF(stderr, " done in %g sec : %g Mptcl/sec\n", get_time()-t1, this->localTree.n/1e6/(get_time()-t1));

        this->allocateTreePropMemory(this->localTree);
        this->compute_properties(this->localTree);


        #ifdef DO_BLOCK_TIMESTEP
                devContext.startTiming(execStream->s());
                setActiveGrpsFunc(this->localTree);
                devContext.stopTiming("setActiveGrpsFunc", 10, execStream->s());
                idata.Nact_since_last_tree_rebuild = 0;
        #endif

        idata.lastBuildTime   = get_time() - t1;
        idata.totalBuildTime += idata.lastBuildTime;  
      }
      else
      {
        #ifdef DO_BLOCK_TIMESTEP
          devContext.startTiming(execStream->s());
          setActiveGrpsFunc(this->localTree);
          devContext.stopTiming("setActiveGrpsFunc", 10, execStream->s());      
          idata.Nact_since_last_tree_rebuild = 0;
        #endif        
        //Don't rebuild only update the current boxes
        this->compute_properties(this->localTree);

      }//end rebuild tree

      //Approximate gravity
      t1 = get_time();
      //devContext.startTiming(gravStream->s());
      approximate_gravity(this->localTree);
//      devContext.stopTiming("Approximation", 4, gravStream->s());

      runningLETTimeSum = 0;

      if(nProcs > 1) makeLET();
    }//else if useDirectGravity

    gravStream->sync(); //Syncs the gravity stream, including any gravity computations due to LET actions

    idata.lastGravTime      = get_time() - t1;
    idata.totalGravTime    += idata.lastGravTime;
    idata.lastLETCommTime   = thisPartLETExTime;
    idata.totalLETCommTime += thisPartLETExTime;


    //Compute the total number of interactions that we executed
    tTempTime = get_time();
#if 1
   localTree.interactions.d2h();

   long long directSum = 0;
   long long apprSum = 0;

   for(int i=0; i < localTree.n; i++)
   {
     apprSum     += localTree.interactions[i].x;
     directSum   += localTree.interactions[i].y;
   }
   char buff2[512];
   sprintf(buff2, "INT Interaction at (rank= %d ) iter: %d\tdirect: %llu\tappr: %llu\tavg dir: %f\tavg appr: %f\n",
                   procId,iter, directSum ,apprSum, directSum / (float)localTree.n, apprSum / (float)localTree.n);
   devContext.writeLogEvent(buff2);
#endif
   LOGF(stderr,"Stats calculation took: %lg \n", get_time()-tTempTime);


    float ms=0, msLET=0;
#if 1 //enable when load-balancing, gets the accurate GPU time from events
    CU_SAFE_CALL(cudaEventElapsedTime(&ms, startLocalGrav, endLocalGrav));
    if(nProcs > 1)  CU_SAFE_CALL(cudaEventElapsedTime(&msLET,startRemoteGrav, endRemoteGrav));

    msLET += runningLETTimeSum;

    char buff[512];
    sprintf(buff,  "APPTIME [%d]: Iter: %d\t%g \tn: %d EventTime: %f  and %f\tSum: %f\n",
        procId, iter, idata.lastGravTime, this->localTree.n, ms, msLET, ms+msLET);
    LOGF(stderr,"%s", buff);
    devContext.writeLogEvent(buff);
#else
    ms    = 1;
    msLET = 1;
#endif

    idata.lastGPUGravTimeLocal   = ms;
    idata.lastGPUGravTimeLET     = msLET;
    idata.totalGPUGravTimeLocal += ms;
    idata.totalGPUGravTimeLET   += msLET;

    //Different options for basing the load balance on
    lastLocal = ms;
    lastTotal = ms + msLET;    

//    lastLocal = 1;//JB1 REMOVE TODO NOTE
//    lastTotal = 1;//JB1 REMOVE TODO NOTE

    //Corrector
    tTempTime = get_time();
    devContext.startTiming(execStream->s());
    correct(this->localTree);
    devContext.stopTiming("Correct", 8, execStream->s());
    idata.totalPredCor += get_time() - tTempTime;


    
    if(nProcs > 1)
    {
      #ifdef USE_MPI
      //Wait on all processes and time how long the waiting took
      t1 = get_time();
      devContext.startTiming(execStream->s());
      //Gather info about the load-balance, used to decide if we need to refine the domains
      MPI_Allreduce(&lastTotal, &maxExecTimePrevStep, 1, MPI_FLOAT, MPI_MAX, mpiCommWorld);
      MPI_Allreduce(&lastTotal, &avgExecTimePrevStep, 1, MPI_FLOAT, MPI_SUM, mpiCommWorld);
      avgExecTimePrevStep /= nProcs;

      devContext.stopTiming("Unbalance", 12, execStream->s());
      idata.lastWaitTime  += get_time() - t1;
      idata.totalWaitTime += idata.lastWaitTime;
      #endif
    }
    
    idata.Nact_since_last_tree_rebuild += this->localTree.n_active_particles;

    //Compute energies
    tTempTime = get_time();
    devContext.startTiming(execStream->s());
    double de = compute_energies(this->localTree);
    devContext.stopTiming("Energy", 7, execStream->s());
    idata.totalPredCor += get_time() - tTempTime;

    if(statisticsIter > 0)
    {
      if(t_current >= nextStatsTime)
      {
        nextStatsTime += statisticsIter;
        double tDens0 = get_time();
        localTree.bodies_pos.d2h();
        localTree.bodies_vel.d2h();
        localTree.bodies_ids.d2h();

        double tDens1 = get_time();
        const DENSITY dens(mpiCommWorld, procId, nProcs, localTree.n,
                           &localTree.bodies_pos[0],
                           &localTree.bodies_vel[0],
                           &localTree.bodies_ids[0],
                           1, 2.33e9, 20, "density", t_current);

        double tDens2 = get_time();
        if(procId == 0) LOGF(stderr,"Density took: Copy: %lg Create: %lg \n", tDens1-tDens0, tDens2-tDens1);

        double tDisk1 = get_time();
        const DISKSTATS diskstats(mpiCommWorld, procId, nProcs, localTree.n,
                           &localTree.bodies_pos[0],
                           &localTree.bodies_vel[0],
                           &localTree.bodies_ids[0],
                           1, 2.33e9, "diskstats", t_current);

        double tDisk2 = get_time();
        if(procId == 0) LOGF(stderr,"Diskstats took: Create: %lg \n", tDisk2-tDisk1);
      }
    }//Statistics dumping


    if (useMPIIO)
    {
      if (mpiRenderMode)
        dumpDataMPI();
      else
        dumpData();
    }
    else if (snapshotIter > 0)
    {
      if((t_current >= nextSnapTime))
      {
        nextSnapTime += snapshotIter;

        while(!ioSharedData.writingFinished)
        {
          fprintf(stderr,"Waiting till previous snapshot has been written\n");
          usleep(100); //Wait till previous snapshot is written
        }

        ioSharedData.t_current  = t_current;

        //TODO JB, why do we do malloc here?
        assert(ioSharedData.nBodies == 0);
        ioSharedData.malloc(localTree.n);


        localTree.bodies_pos.d2h(localTree.n, ioSharedData.Pos);
        localTree.bodies_vel.d2h(localTree.n, ioSharedData.Vel);
        localTree.bodies_ids.d2h(localTree.n, ioSharedData.IDs);
        ioSharedData.writingFinished = false;
        if(nProcs <= 16) while (!ioSharedData.writingFinished);
      }
    }


    if (iter >= iterEnd) return true;

    if(t_current >= tEnd)
    {
      compute_energies(this->localTree);
      double totalTime = get_time() - idata.startTime;
      LOG("Finished: %f > %f \tLoop alone took: %f\n", t_current, tEnd, totalTime);
      my_dev::base_mem::printMemUsage();
      return true;
    }
    iter++; 

    return false;
}


void octree::iterate_setup(IterationData &idata) {
  
  if(execStream == NULL)          execStream          = new my_dev::dev_stream(0);
  if(gravStream == NULL)          gravStream          = new my_dev::dev_stream(0);
  if(copyStream == NULL)          copyStream          = new my_dev::dev_stream(0);
  if(LETDataToHostStream == NULL) LETDataToHostStream = new my_dev::dev_stream(0);

  CU_SAFE_CALL(cudaEventCreate(&startLocalGrav));
  CU_SAFE_CALL(cudaEventCreate(&endLocalGrav));
  CU_SAFE_CALL(cudaEventCreate(&startRemoteGrav));
  CU_SAFE_CALL(cudaEventCreate(&endRemoteGrav));

  devContext.writeLogEvent("Start execution\n");
  
  //Setup of the multi-process particle distribution, initially it should be equal
  #ifdef USE_MPI
    if(nProcs > 1)
    {
      for(int i=0; i < 5; i++)
      {
        double notUsed     = 0;
        int maxN = 0, minN = 0;
        sort_bodies(localTree, true, true); //Initial sort to get global boundaries to compute keys
        parallelDataSummary(localTree, 30, 30, notUsed, notUsed, true); //1 for all process, equal part distribution

        //Check if the min/max are within certain percentage
        MPI_Allreduce(&localTree.n, &maxN, 1, MPI_INT, MPI_MAX, mpiCommWorld);
        MPI_Allreduce(&localTree.n, &minN, 1, MPI_INT, MPI_MIN, mpiCommWorld);

        //Compute difference in percent
        int perc = (int)(100*(maxN-minN)/(double)minN);

        if(procId == 0)
        {
          LOGF(stderr, "Particle setup iteration: %d Min: %d  Max: %d Diff: %d %%\n", i, minN, maxN, perc);
        }
        if(perc < 10) break; //We're happy if difference is less than 10%
      }
    }
  #endif

  letRunning      = false;
  idata.startTime = get_time();
}

void octree::iterate_teardown(IterationData &idata) {
  if(execStream != NULL) {
    delete execStream;
    execStream = NULL;
  }

  if(gravStream != NULL) {
    delete gravStream;
    gravStream = NULL;
  }

  if(copyStream != NULL) {
    delete copyStream;
    copyStream = NULL;
  }

  if(LETDataToHostStream != NULL)  {
    delete LETDataToHostStream;
    LETDataToHostStream = NULL;
  }
}

void octree::iterate() {
  IterationData idata;
  iterate_setup(idata);

  while(true)
  {
    bool stopRun = iterate_once(idata);

    double totalTime = get_time() - idata.startTime;

    static char textBuff[16384];
    sprintf(textBuff,"TIME [%02d] TOTAL: %g\t Grav: %g (GPUgrav %g , LET Com: %g)\tBuild: %g\tDomain: %g\t Wait: %g\tdomUp: %g\tdomEx: %g\tdomWait: %g\ttPredCor: %g\n",
                      procId, totalTime, idata.totalGravTime,
                      (idata.totalGPUGravTimeLocal+idata.totalGPUGravTimeLET) / 1000,
                      idata.totalLETCommTime,
                      idata.totalBuildTime, idata.totalDomTime, idata.lastWaitTime,
                      idata.totalDomUp, idata.totalDomEx, idata.totalDomWait, idata.totalPredCor);

    if (procId == 0)
    {
      LOGF(stderr,"%s", textBuff);
      LOGF(stdout,"%s", textBuff);
    }
    devContext.writeLogEvent(textBuff);
    this->writeLogToFile();     //Write the logdata to file

    if(stopRun) break;
  } //end while
  
  iterate_teardown(idata);
} //end iterate


void octree::predict(tree_structure &tree)
{
  //Functions that predicts the particles to the next timestep

  //tend is time per particle
  //tnext is reduce result

  //First we get the minimum time, which is the next integration time
  #ifdef DO_BLOCK_TIMESTEP
    int blockSize = NBLOCK_REDUCE ;
    getTNext.set_arg<int>(0,    &tree.n);
    getTNext.set_arg<cl_mem>(1, tree.bodies_time.p());
    getTNext.set_arg<cl_mem>(2, tnext.p());
    getTNext.set_arg<float>(3,  NULL, 128); //Dynamic shared memory
    getTNext.setWork(-1, 128, blockSize);
    getTNext.execute(execStream->s());

    //This will not work in block-step! Only shared- time step
    //in block step we need syncs and global communication
    if(tree.n == 0)
    {
      t_previous =  t_current;
      t_current  += timeStep;
    }
    else
    {
      //Reduce the last parts on the host
      tnext.d2h();
      t_previous = t_current;
      t_current  = tnext[0];
      for (int i = 1; i < blockSize ; i++)
      {
          t_current = std::min(t_current, tnext[i]);
      }
    }
//    tree.activeGrpList.zeroMem();      //Reset the active grps
  #else
    static int temp = 0;
    t_previous =  t_current;
    if(temp > 0)
      t_current  += timeStep;
    else
       temp = 1;
  #endif


    
  //Set valid list to zero
  predictParticles.set_arg<int>(0,    &tree.n);
  predictParticles.set_arg<float>(1,  &t_current);
  predictParticles.set_arg<float>(2,  &t_previous);
  predictParticles.set_arg<cl_mem>(3, tree.bodies_pos.p());
  predictParticles.set_arg<cl_mem>(4, tree.bodies_vel.p());
  predictParticles.set_arg<cl_mem>(5, tree.bodies_acc0.p());
  predictParticles.set_arg<cl_mem>(6, tree.bodies_time.p());
  predictParticles.set_arg<cl_mem>(7, tree.bodies_Ppos.p());
  predictParticles.set_arg<cl_mem>(8, tree.bodies_Pvel.p());  
  predictParticles.setWork(tree.n, 128);
  predictParticles.execute(execStream->s());
  

  #ifdef DO_BLOCK_TIMESTEP
    //Compact the valid list to get a list of valid groups
//    gpuCompact(devContext, tree.activeGrpList, tree.active_group_list,
//              tree.n_groups, &tree.n_active_groups);
  #else
//    tree.n_active_groups = tree.n_groups;
  #endif


//  LOG("t_previous: %lg t_current: %lg dt: %lg Active groups: %d \n",
//         t_previous, t_current, t_current-t_previous, tree.n_active_groups);
  
} //End predict


void octree::setActiveGrpsFunc(tree_structure &tree)
{
  //Moved to compute_properties
}

void octree::direct_gravity(tree_structure &tree)
{
  directGrav.set_arg<cl_mem>(0, tree.bodies_acc1.p());
  directGrav.set_arg<cl_mem>(1, tree.bodies_Ppos.p());
  directGrav.set_arg<cl_mem>(2, tree.bodies_Ppos.p());
  directGrav.set_arg<int>(3,    &tree.n);
  directGrav.set_arg<int>(4,    &tree.n);
  directGrav.set_arg<float>(5,  &(this->eps2));
  directGrav.set_arg<float4>(6, NULL, 256);
  std::vector<size_t> localWork(2), globalWork(2);
  localWork[0] = 256; localWork[1] = 1;
  globalWork[0] = 256 * ((tree.n + 255) / 256);
  globalWork[1] = 1;
  directGrav.setWork(globalWork, localWork);
  directGrav.execute(gravStream->s());  //First half
}

void octree::approximate_gravity(tree_structure &tree)
{ 
  uint2 node_begend;
  int level_start = tree.startLevelMin;
  node_begend.x   = tree.level_list[level_start].x;
  node_begend.y   = tree.level_list[level_start].y;

  tree.activePartlist.zeroMemGPUAsync(gravStream->s());
  LOG("node begend: %d %d iter-> %d\n", node_begend.x, node_begend.y, iter);

  //Set the kernel parameters, many!
  approxGrav.set_arg<int>(0,    &tree.n_active_groups);
  approxGrav.set_arg<int>(1,    &tree.n);
  approxGrav.set_arg<float>(2,  &(this->eps2));
  approxGrav.set_arg<uint2>(3,  &node_begend);
  approxGrav.set_arg<cl_mem>(4,  tree.active_group_list.p());
  approxGrav.set_arg<cl_mem>(5,  tree.bodies_Ppos.p());
  approxGrav.set_arg<cl_mem>(6,  tree.multipole.p());
  approxGrav.set_arg<cl_mem>(7,  tree.bodies_acc1.p());
  approxGrav.set_arg<cl_mem>(8,  tree.bodies_Ppos.p());
  approxGrav.set_arg<cl_mem>(9,  tree.ngb.p());
  approxGrav.set_arg<cl_mem>(10, tree.activePartlist.p());
  approxGrav.set_arg<cl_mem>(11, tree.interactions.p());
  approxGrav.set_arg<cl_mem>(12, tree.boxSizeInfo.p());
  approxGrav.set_arg<cl_mem>(13, tree.groupSizeInfo.p());
  approxGrav.set_arg<cl_mem>(14, tree.boxCenterInfo.p());
  approxGrav.set_arg<cl_mem>(15, tree.groupCenterInfo.p());
  approxGrav.set_arg<cl_mem>(16, tree.bodies_Pvel.p());
  approxGrav.set_arg<cl_mem>(17, tree.generalBuffer1.p());  //The buffer to store the tree walks
  approxGrav.set_arg<cl_mem>(18, tree.bodies_h.p());        //Per particle search radius
  approxGrav.set_arg<cl_mem>(19, tree.bodies_dens.p());     //Per particle density (x) and nnb (y)
  approxGrav.set_arg<real4>(20,  tree.boxSizeInfo,   4, "texNodeSize");
  approxGrav.set_arg<real4>(21,  tree.boxCenterInfo, 4, "texNodeCenter");
  approxGrav.set_arg<real4>(22,  tree.multipole,     4, "texMultipole");
  approxGrav.set_arg<real4>(23,  tree.bodies_Ppos,   4, "texBody");
    
  approxGrav.setWork(-1, NTHREAD, nBlocksForTreeWalk);

  cudaEventRecord(startLocalGrav, gravStream->s());
  approxGrav.execute(gravStream->s());  //First half
  cudaEventRecord(endLocalGrav, gravStream->s());

#if 0
	//Print density information
	tree.bodies_dens.d2h();
	tree.bodies_pos.d2h();
	tree.bodies_h.d2h();

	int nnbMin = 10e7;
	int nnbMax = -10e7;
	int nnbSum = 0;

	static bool firstIter0 = true;
	for(int i=0; i < tree.n; i++)
	{
		float r = sqrt(pow(tree.bodies_pos[i].x,2) + pow(tree.bodies_pos[i].y, 2) + pow(tree.bodies_pos[i].z,2));

		nnbMin =  std::min(nnbMin, (int)tree.bodies_dens[i].y);
		nnbMax =  std::max(nnbMax, (int)tree.bodies_dens[i].y);
		nnbSum += (int)tree.bodies_dens[i].y;
if(firstIter0 == true || iter == 40){
		fprintf(stderr, "DENS Iter: %d\t%d\t%f\t%f\t%f\tr: %f\th: %f\td: %f\tnnb: %f\t logs: %f %f  \n",
			iter,
			i, tree.bodies_pos[i].x, tree.bodies_pos[i].y, tree.bodies_pos[i].z,
			r, 
			tree.bodies_h[i],
			tree.bodies_dens[i].x, tree.bodies_dens[i].y,
			log10(tree.bodies_dens[i].x), log2(tree.bodies_dens[i].x)
			);
}

	}
		firstIter0 = false;
		fprintf(stderr,"STATD Iter: %d\tMin: %d\tMax: %d\tAvg: %f\n", iter, nnbMin, nnbMax, nnbSum / (float)tree.n);
//	exit(0);
#endif  



  //Print interaction statistics
  #if 0
  
  tree.body2group_list.d2h();
  tree.interactions.d2h();
    long long directSum = 0;
    long long apprSum = 0;
    long long directSum2 = 0;
    long long apprSum2 = 0;
    
    
    int maxDir = -1;
    int maxAppr = -1;

    for(int i=0; i < tree.n; i++)
    {
      apprSum     += tree.interactions[i].x;
      directSum   += tree.interactions[i].y;
      
      maxAppr = max(maxAppr,tree.interactions[i].x);
      maxDir  = max(maxDir,tree.interactions[i].y);
      
      apprSum2     += tree.interactions[i].x*tree.interactions[i].x;
      directSum2   += tree.interactions[i].y*tree.interactions[i].y;   
      
//      if(i < 35)
//      fprintf(stderr, "%d\t Direct: %d\tApprox: %d\t Group: %d \n",
//              i, tree.interactions[i].y, tree.interactions[i].x,
//              tree.body2group_list[i]);
    }
    cout << "Interaction at (rank= " << mpiGetRank() << " ) iter: " << iter << "\tdirect: " << directSum << "\tappr: " << apprSum << "\t";
    cout << "avg dir: " << directSum / tree.n << "\tavg appr: " << apprSum / tree.n << "\tMaxdir: " << maxDir << "\tmaxAppr: " << maxAppr <<  endl;
    cout << "sigma dir: " << sqrt((directSum2  - directSum)/ tree.n) << "\tsigma appr: " << std::sqrt((apprSum2 - apprSum) / tree.n)  <<  endl;    

  #endif
  
  
  if(mpiGetNProcs() == 1) //Only do it here if there is only one process
  {
   //#ifdef DO_BLOCK_TIMESTEP
  #if 0 //Demo mode
      //Reduce the number of valid particles    
      getNActive.set_arg<int>(0,    &tree.n);
      getNActive.set_arg<cl_mem>(1, tree.activePartlist.p());
      getNActive.set_arg<cl_mem>(2, this->nactive.p());
      getNActive.set_arg<int>(3,    NULL, 128); //Dynamic shared memory , equal to number of threads
      getNActive.setWork(-1, 128,   NBLOCK_REDUCE);
      
      //JB Need a sync here This is required otherwise the gravity overlaps the reduction
      //and we get incorrect numbers. 
      //Note Disabled this whole function for demo!
      gravStream->sync(); 
      getNActive.execute(execStream->s());
      
      

      //Reduce the last parts on the host
      this->nactive.d2h();
      tree.n_active_particles = this->nactive[0];
      for (int i = 1; i < NBLOCK_REDUCE ; i++)
          tree.n_active_particles += this->nactive[i];

      LOG("Active particles: %d \n", tree.n_active_particles);
    #else
      tree.n_active_particles = tree.n;
      LOG("Active particles: %d \n", tree.n_active_particles);
    #endif
  }
}
//end approximate


void octree::approximate_gravity_let(tree_structure &tree, tree_structure &remoteTree, int bufferSize, bool doActiveParticles)
{
  //Start and end node of the remote tree structure
  uint2 node_begend;  
  node_begend.x =  0;
  node_begend.y =  remoteTree.remoteTreeStruct.w;
  
  //The texture offset used:
  int nodeTexOffset     = remoteTree.remoteTreeStruct.z ;
  
  //The start and end of the top nodes:
  node_begend.x = (remoteTree.remoteTreeStruct.w >> 16);
  node_begend.y = (remoteTree.remoteTreeStruct.w & 0xFFFF);  
 
  //Number of particles and number of nodes in the remote tree
  int remoteP = remoteTree.remoteTreeStruct.x;
  int remoteN = remoteTree.remoteTreeStruct.y;

  LOG("LET node begend [%d]: %d %d iter-> %d\n", procId, node_begend.x, node_begend.y, iter);

  //Set the kernel parameters, many!
  approxGravLET.set_arg<int>(0,    &tree.n_active_groups);
  approxGravLET.set_arg<int>(1,    &tree.n);
  approxGravLET.set_arg<float>(2,  &(this->eps2));
  approxGravLET.set_arg<uint2>(3,  &node_begend);
  approxGravLET.set_arg<cl_mem>(4, tree.active_group_list.p());
  approxGravLET.set_arg<cl_mem>(5, remoteTree.fullRemoteTree.p());

  void *multiLoc = remoteTree.fullRemoteTree.a(1*(remoteP) + 2*(remoteN+nodeTexOffset));
  approxGravLET.set_arg<cl_mem>(6, &multiLoc);  

  approxGravLET.set_arg<cl_mem>(7, tree.bodies_acc1.p());
  approxGravLET.set_arg<cl_mem>(8, tree.bodies_Ppos.p());
  approxGravLET.set_arg<cl_mem>(9, tree.ngb.p());
  approxGravLET.set_arg<cl_mem>(10, tree.activePartlist.p());
  approxGravLET.set_arg<cl_mem>(11, tree.interactions.p());
  
  void *boxSILoc = remoteTree.fullRemoteTree.a(1*(remoteP));
  approxGravLET.set_arg<cl_mem>(12, &boxSILoc);  

  approxGravLET.set_arg<cl_mem>(13, tree.groupSizeInfo.p());

  void *boxCILoc = remoteTree.fullRemoteTree.a(1*(remoteP) + remoteN + nodeTexOffset);
  approxGravLET.set_arg<cl_mem>(14, &boxCILoc);  

  approxGravLET.set_arg<cl_mem>(15, tree.groupCenterInfo.p());  
  

  approxGravLET.set_arg<cl_mem>(16, tree.bodies_Pvel.p()); //<- Predicted local body velocity
  approxGravLET.set_arg<cl_mem>(17, tree.generalBuffer1.p()); //<- Predicted local body velocity, TODO figure out what this is
  approxGravLET.set_arg<cl_mem>(18, tree.bodies_h.p());    //Per particle search radius
  approxGravLET.set_arg<cl_mem>(19, tree.bodies_dens.p()); //Per particle density (x) and nnb (y)
  
  approxGravLET.set_arg<real4>(20, remoteTree.fullRemoteTree, 4, "texNodeSize",
                               1*(remoteP), remoteN );
  approxGravLET.set_arg<real4>(21, remoteTree.fullRemoteTree, 4, "texNodeCenter",
                               1*(remoteP) + (remoteN + nodeTexOffset), remoteN);
  approxGravLET.set_arg<real4>(22, remoteTree.fullRemoteTree, 4, "texMultipole",
                               1*(remoteP) + 2*(remoteN + nodeTexOffset), 3*remoteN);
  approxGravLET.set_arg<real4>(23, remoteTree.fullRemoteTree, 4, "texBody", 0, remoteP);  

  approxGravLET.setWork(-1, NTHREAD, nBlocksForTreeWalk);
    
  if(letRunning)
  {
    //don't want to overwrite the data of previous LET tree
    gravStream->sync();

    //Add the time to the time sum for the let
    float msLET;
    CU_SAFE_CALL(cudaEventElapsedTime(&msLET,startRemoteGrav, endRemoteGrav));
    runningLETTimeSum += msLET;
  }
  
  remoteTree.fullRemoteTree.h2d(bufferSize); //Only copy required data
  tree.activePartlist.zeroMemGPUAsync(gravStream->s()); //Resets atomics

  CU_SAFE_CALL(cudaEventRecord(startRemoteGrav, gravStream->s()));
  approxGravLET.execute(gravStream->s());
  CU_SAFE_CALL(cudaEventRecord(endRemoteGrav, gravStream->s()));
  letRunning = true;


 //Print interaction statistics
  #if 0
    tree.interactions.d2h();
//     tree.body2group_list.d2h();
    
    long long directSum = 0;
    long long apprSum = 0;
    
    int maxDir = -1;
    int maxAppr = -1;
    
    long long directSum2 = 0;
    long long apprSum2 = 0;
    
    
    for(int i=0; i < tree.n; i++)
    {
      apprSum     += tree.interactions[i].x;
      directSum   += tree.interactions[i].y;
      
      maxAppr = max(maxAppr,tree.interactions[i].x);
      maxDir  = max(maxDir, tree.interactions[i].y);
      
      apprSum2     += (tree.interactions[i].x*tree.interactions[i].x);
      directSum2   += (tree.interactions[i].y*tree.interactions[i].y);    
    }

    cout << "Interaction (LET) at (rank= " << mpiGetRank() << " ) iter: " << iter << "\tdirect: " << directSum << "\tappr: " << apprSum << "\t";
    cout << "avg dir: " << directSum / tree.n << "\tavg appr: " << apprSum / tree.n  << "\tMaxdir: " << maxDir << "\tmaxAppr: " << maxAppr <<  endl;
    cout << "sigma dir: " << sqrt((directSum2  - directSum)/ tree.n) << "\tsigma appr: " << std::sqrt((apprSum2 - apprSum) / tree.n)  <<  endl;
  #endif
    
  if(doActiveParticles) //Only do it here if there is only one process
  {
   //#ifdef DO_BLOCK_TIMESTEP  
  #if 0 //Demo mode
      //Reduce the number of valid particles    
      getNActive.set_arg<int>(0,    &tree.n);
      getNActive.set_arg<cl_mem>(1, tree.activePartlist.p());
      getNActive.set_arg<cl_mem>(2, this->nactive.p());
      getNActive.set_arg<int>(3,    NULL, 128); //Dynamic shared memory , equal to number of threads
      getNActive.setWork(-1, 128,   NBLOCK_REDUCE);
      
      //JB Need a sync here This is required otherwise the gravity overlaps the reduction
      //and we get incorrect numbers. 
      //Note Disabled this whole function for demo!
      gravStream->sync(); 
      getNActive.execute(execStream->s());
      
      

      //Reduce the last parts on the host
      this->nactive.d2h();
      tree.n_active_particles = this->nactive[0];
      for (int i = 1; i < NBLOCK_REDUCE ; i++)
          tree.n_active_particles += this->nactive[i];

      LOG("Active particles: %d \n", tree.n_active_particles);
    #else
      tree.n_active_particles = tree.n;
      LOG("Active particles: %d \n", tree.n_active_particles);
    #endif
  }    
}
//end approximate



void octree::correct(tree_structure &tree)
{ 
  //TODO this might be moved to the gravity call where we have that info anyway?
  tree.n_active_particles = tree.n;
  #ifdef DO_BLOCK_TIMESTEP
    //Reduce the number of valid particles
    gravStream->sync(); //Sync to make sure that the gravity phase is finished
    getNActive.set_arg<int>(0,    &tree.n);
    getNActive.set_arg<cl_mem>(1, tree.activePartlist.p());
    getNActive.set_arg<cl_mem>(2, this->nactive.p());
    getNActive.set_arg<int>(3,    NULL, 128); //Dynamic shared memory , equal to number of threads
    getNActive.setWork(-1, 128,   NBLOCK_REDUCE);
    getNActive.execute(execStream->s());

    //Reduce the last parts on the host
    this->nactive.d2h();
    tree.n_active_particles = this->nactive[0];
    for (int i = 1; i < NBLOCK_REDUCE ; i++)
        tree.n_active_particles += this->nactive[i];
  #endif
  LOG("Active particles: %d \n", tree.n_active_particles);





  my_dev::dev_mem<float2>  float2Buffer(devContext);
  my_dev::dev_mem<real4>   real4Buffer1(devContext);

  int memOffset = float2Buffer.cmalloc_copy(tree.generalBuffer1, tree.n, 0);
      memOffset = real4Buffer1.cmalloc_copy(tree.generalBuffer1, tree.n, memOffset);
  
 
  correctParticles.set_arg<int   >(0, &tree.n);
  correctParticles.set_arg<float >(1, &t_current);
  correctParticles.set_arg<cl_mem>(2, tree.bodies_time.p());
  correctParticles.set_arg<cl_mem>(3, tree.activePartlist.p());
  correctParticles.set_arg<cl_mem>(4, tree.bodies_vel.p());
  correctParticles.set_arg<cl_mem>(5, tree.bodies_acc0.p());
  correctParticles.set_arg<cl_mem>(6, tree.bodies_acc1.p());
  correctParticles.set_arg<cl_mem>(7, tree.bodies_h.p());
  correctParticles.set_arg<cl_mem>(8, tree.bodies_dens.p());
  correctParticles.set_arg<cl_mem>(9, tree.bodies_pos.p());
  correctParticles.set_arg<cl_mem>(10, tree.bodies_Ppos.p());
  correctParticles.set_arg<cl_mem>(11, tree.bodies_Pvel.p());
  correctParticles.set_arg<cl_mem>(12, tree.oriParticleOrder.p());
  correctParticles.set_arg<cl_mem>(13, real4Buffer1.p());
  correctParticles.set_arg<cl_mem>(14, float2Buffer.p());

#if 1
  //Buffers required for storing the position of selected particles
  correctParticles.set_arg<cl_mem>(15, tree.bodies_ids.p());
  correctParticles.set_arg<cl_mem>(16, specialParticles.p());

#endif

  correctParticles.setWork(tree.n, 128);
  correctParticles.execute(execStream->s());
 
  //Copy the reshuffled items back to their original buffers
  tree.bodies_acc0.copy_devonly(real4Buffer1, tree.n);
  tree.bodies_time.copy_devonly(float2Buffer, float2Buffer.get_size());


  #ifdef DO_BLOCK_TIMESTEP
    computeDt.set_arg<int>(0,    &tree.n);
    computeDt.set_arg<float>(1,  &t_current);
    computeDt.set_arg<float>(2,  &(this->eta));
    computeDt.set_arg<int>(3,    &(this->dt_limit));
    computeDt.set_arg<float>(4,  &(this->eps2));
    computeDt.set_arg<cl_mem>(5, tree.bodies_time.p());
    computeDt.set_arg<cl_mem>(6, tree.bodies_vel.p());
    computeDt.set_arg<cl_mem>(7, tree.ngb.p());
    computeDt.set_arg<cl_mem>(8, tree.bodies_pos.p());
    computeDt.set_arg<cl_mem>(9, tree.bodies_acc0.p());
    computeDt.set_arg<cl_mem>(10, tree.activePartlist.p());
    computeDt.set_arg<float >(11, &timeStep);

    computeDt.setWork(tree.n, 128);
    computeDt.execute(execStream->s());
  #endif
}



 //Double precision
double octree::compute_energies(tree_structure &tree)
{
  Ekin = 0.0; Epot = 0.0;

  #if 0
    double hEkin = 0.0;
    double hEpot = 0.0;

    tree.bodies_pos.d2h();
    tree.bodies_vel.d2h();
    tree.bodies_acc0.d2h();
    for (int i = 0; i < tree.n; i++) {
      float4 vel = tree.bodies_vel[i];
      hEkin += tree.bodies_pos[i].w*0.5*(vel.x*vel.x +
                                 vel.y*vel.y +
                                 vel.z*vel.z);
      hEpot += tree.bodies_pos[i].w*0.5*tree.bodies_acc0[i].w;
    }
    MPI_Barrier(mpiCommWorld);
    double hEtot = hEpot + hEkin;
    LOG("Energy (on host): Etot = %.10lg Ekin = %.10lg Epot = %.10lg \n", hEtot, hEkin, hEpot);
  #endif

  //float2 energy : x is kinetic energy, y is potential energy
  int blockSize = NBLOCK_REDUCE ;
  my_dev::dev_mem<double2>  energy(devContext);
  energy.cmalloc_copy(tree.generalBuffer1, blockSize, 0);
  

    
  computeEnergy.set_arg<int>(0,    &tree.n);
  computeEnergy.set_arg<cl_mem>(1, tree.bodies_pos.p());
  computeEnergy.set_arg<cl_mem>(2, tree.bodies_vel.p());
  computeEnergy.set_arg<cl_mem>(3, tree.bodies_acc0.p());
  computeEnergy.set_arg<cl_mem>(4, energy.p());
  computeEnergy.set_arg<double>(5, NULL, 128*2); //Dynamic shared memory, equal to number of threads times 2

  computeEnergy.setWork(-1, 128, blockSize);
  computeEnergy.execute(execStream->s());

  //Reduce the last parts on the host
  energy.d2h();
  Ekin = energy[0].x;
  Epot = energy[0].y;
  for (int i = 1; i < blockSize ; i++)
  {
      Ekin += energy[i].x;
      Epot += energy[i].y;
  }
  
  //Sum the values / energies of the system using MPI
  AllSum(Epot); AllSum(Ekin);
  
  Etot = Epot + Ekin;

  if (store_energy_flag) {
    Ekin0 = Ekin;
    Epot0 = Epot;
    Etot0 = Etot;
    Ekin1 = Ekin;
    Epot1 = Epot;
    Etot1 = Etot;
    tinit = get_time();
    store_energy_flag = false;
  }

  
  double de  = (Etot - Etot0)/Etot0;
  double dde = (Etot - Etot1)/Etot1;

  if(tree.n_active_particles == tree.n)
  {
    de_max  = std::max( de_max, std::abs( de));
    dde_max = std::max(dde_max, std::abs(dde));
  }  
  
  Ekin1 = Ekin;
  Epot1 = Epot;
  Etot1 = Etot;
  
  if(mpiGetRank() == 0)
  {
#if 0
  LOG("iter=%d : time= %lg  Etot= %.10lg  Ekin= %lg   Epot= %lg : de= %lg ( %lg ) d(de)= %lg ( %lg ) t_sim=  %lg sec\n",
		  iter, this->t_current, Etot, Ekin, Epot, de, de_max, dde, dde_max, get_time() - tinit);  
  LOGF(stderr, "iter=%d : time= %lg  Etot= %.10lg  Ekin= %lg   Epot= %lg : de= %lg ( %lg ) d(de)= %lg ( %lg ) t_sim=  %lg sec\n", 
		  iter, this->t_current, Etot, Ekin, Epot, de, de_max, dde, dde_max, get_time() - tinit);          
#else
  printf("iter=%d : time= %lg  Etot= %.10lg  Ekin= %lg   Epot= %lg : de= %lg ( %lg ) d(de)= %lg ( %lg ) t_sim=  %lg sec\n",
		  iter, this->t_current, Etot, Ekin, Epot, de, de_max, dde, dde_max, get_time() - tinit);  
  fprintf(stderr, "iter=%d : time= %lg  Etot= %.10lg  Ekin= %lg   Epot= %lg : de= %lg ( %lg ) d(de)= %lg ( %lg ) t_sim=  %lg sec\n", 
		  iter, this->t_current, Etot, Ekin, Epot, de, de_max, dde, dde_max, get_time() - tinit);          
#endif
  }

  return de;
}


#if 0
//TODO JB remove this function
void octree::checkRemovalDistance(tree_structure &tree)
{
  //Download all particle properties to the host

  tree.bodies_pos.d2h();    //The particles positions
  tree.bodies_key.d2h();    //The particles keys
  tree.bodies_vel.d2h();    //Velocities
  tree.bodies_acc0.d2h();    //Acceleration
  tree.bodies_acc1.d2h();    //Acceleration
  tree.bodies_time.d2h();  //The timestep details (.x=tb, .y=te
  tree.bodies_ids.d2h();

  bool modified = false;

  tree.multipole.d2h();
  real4 com = tree.multipole[0];

  int storeIdx = 0;

  int NTotalT = 0, NFirstT = 0, NSecondT = 0, NThirdT = 0;

  for(int i=0; i < tree.n ; i++)
  {
    real4 posi = tree.bodies_pos[i];

    real4 r;
    r.x = (posi.x-com.x); r.y = (posi.y-com.y);r.z = (posi.z-com.z);
    float dist = (r.x*r.x) + (r.y*r.y) + (r.z*r.z);
    dist = sqrt(dist);

    tree.bodies_pos[storeIdx] = tree.bodies_pos[i];
    tree.bodies_key[storeIdx] = tree.bodies_key[i];
    tree.bodies_vel[storeIdx] = tree.bodies_vel[i];
    tree.bodies_acc0[storeIdx] = tree.bodies_acc0[i];
    tree.bodies_acc1[storeIdx] = tree.bodies_acc1[i];
    tree.bodies_time[storeIdx] = tree.bodies_time[i];
    tree.bodies_ids[storeIdx] = tree.bodies_ids[i];

    if(dist > removeDistance)
    {
        //Remove this particle
        cerr << "Removing particle: " << i << " distance is: " << dist;
        cerr << "\tPOSM: " << posi.x << " " << posi.y << " " << posi.z << " " << posi.w;
        cerr << "\tCOM: " << com.x << " " << com.y << " " << com.z << " " << com.w << endl;

        //Add this particles potential energy to the sum
//         removedPot += hostbodies[i].w*0.5*hostacc0[i].w;
        modified =  true;
    }
    else
    {
      storeIdx++; //Increase the store position

      NTotalT++;
      NFirstT = 0, NSecondT = 0, NThirdT = 0;

      //Specific for Jeroens files
      if(tree.bodies_ids[i] >= 0 && tree.bodies_ids[i] < 100000000) NThirdT++;
      if(tree.bodies_ids[i] >= 100000000 && tree.bodies_ids[i] < 200000000) NSecondT++;
      if(tree.bodies_ids[i] >= 200000000 && tree.bodies_ids[i] < 300000000) NFirstT++;
    }
  } //end for loop


  NTotal  = NTotalT;
  NFirst  = NFirstT;
  NSecond = NSecondT;
  NThird  = NThirdT;


  if(modified)
  {
    tree.setN(storeIdx);

    //Now copy them back
    tree.bodies_pos.h2d();    //The particles positions
    tree.bodies_key.h2d();    //The particles keys
    tree.bodies_vel.h2d();    //Velocities
    tree.bodies_acc0.h2d();    //Acceleration
    tree.bodies_acc1.h2d();    //Acceleration
    tree.bodies_time.h2d();  //The timestep details (.x=tb, .y=te
    tree.bodies_ids.h2d();

    //Compute the energy!
    store_energy_flag = true;
    compute_energies(tree);
  }//end if modified
  else
  {
        cerr << "Nothing removed! :-) \n";
  }

  //TODO sync the number of particles with process 0 for correct header file
}
#endif
