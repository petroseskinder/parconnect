/**
 * @file    labelProp.hpp
 * @ingroup group
 * @author  Chirag Jain <cjain7@gatech.edu>
 * @brief   Connected component labeling using label propagation (or coloring) approach
 *
 * Copyright (c) 2015 Georgia Institute of Technology. All Rights Reserved.
 */

#ifndef LABEL_PROPAGATION_HPP 
#define LABEL_PROPAGATION_HPP

//Includes
#include <iostream>

//Own includes
#include "coloring/tupleComp.hpp"
#include "coloring/labelProp_utils.hpp"
#include "utils/commonfuncs.hpp"
#include "utils/logging.hpp"
#include "utils/prettyprint.hpp"

#include "mxx/sort.hpp"
#include "mxx_extra/sort.hpp"
#include "mxx/timer.hpp"
#include "mxx/comm.hpp"

namespace conn 
{
  namespace coloring
  {

    /**
     * @class                     conn::coloring::ccl
     * @brief                     supports parallel connected component labeling using label propagation technique
     * @tparam[in]  nIdType       type used for node id
     * @tparam[in]  OPTIMIZATION  optimization level for benchmarking, use loadbalanced for the best version 
     * @tparam[in]  DOUBLING      controls whether pointer doubling would be executed or not, 'ON' by default.
     */
    template<typename nIdType = uint64_t, uint8_t OPTIMIZATION = opt_level::loadbalanced, uint8_t DOUBLING = lever::ON>
    class ccl 
    {
      public:
        //Type for saving parition ids
        //Defaulted to uint32_t, assuming we will never have more than 4 Billion partitions in the graph
        using pIdtype = uint32_t;

        //Type for saving node ids
        using nodeIdType = nIdType;

      private:

        //This is the communicator which participates for computing the components
        mxx::comm comm;

        //Number of components
        std::size_t componentCount;

      private:

        using T = std::tuple<pIdtype, pIdtype, nodeIdType>;
        std::vector<T> tupleVector;

        //Used during initialization of <Pn>
        //Also used to mark partitions as stable
        pIdtype MAX_PID = std::numeric_limits<pIdtype>::max();

        //Used to mark tuples as stable 
        //partitions would become stable if all its tuples are stable
        pIdtype MAX_PID2 = std::numeric_limits<pIdtype>::max() -1 ;

        //Used to mark the special tuples used during doubling
        nodeIdType MAX_NID = std::numeric_limits<nodeIdType>::max();

      public:
        /**
         * @brief                 public constructor
         * @param[in] edgeList    distributed vector of edges
         * @param[in] c           mpi communicator for the execution 
         */
        template <typename E>
        ccl(std::vector<std::pair<E,E>> &edgeList, const mxx::comm &c) : comm(c.copy()) 
        {
          //nodeIdType and E should match
          //If they don't, modify the class type or the edgeList type
          static_assert(std::is_same<E, nodeIdType>::value, "types must match");

          //Parse the edgeList
          convertEdgeListforCCL(edgeList);

          //Re-distribute the tuples uniformly across the ranks
          mxx::distribute_inplace(tupleVector, comm);
        }

        /**
         * @brief   Compute the connected component labels
         * @note    Note that the communicator is freed after the computation
         */
        void compute()
        {
          //Size of vector should be >= 0
          assert(tupleVector.begin() != tupleVector.end());

          runConnectedComponentLabeling();

          //Save the component count
          computeComponentCount();

          //Free the communicator
          free_comm();
        }

        /**
         * @brief     count the components in the graph after ccl (useful for debugging/testing)
         * @return    count 
         * @note      should be called after computing connected components. 
         */
        std::size_t getComponentCount()
        {
          return componentCount;
        }

      private:

        /**
         * @brief     Free the communicator
         * @note      Required to make sure that the communicator is freed before MPI_Finalize 
         */
        void free_comm()
        {
          comm.~comm();
        }

        /**
         * @brief     converts the edgelist to vector of tuples needed for ccl
         * @details   For the bucket in the edgeList ...<(u, v1), (u,v2)>...
         *            we append <(u, ~, u), (u, ~, v1), (u, ~, v2)> to our tupleVector
         *            We ignore the bucket splits across ranks here, because that shouldn't affect the correctness and complexity
         */
        template <typename edgeListPairsType>
        void convertEdgeListforCCL(edgeListPairsType &edgeList)
        {
          mxx::section_timer timer(std::cerr, comm);

          //Sort the edgeList by src id of each edge
          mxx::sort(edgeList.begin(), edgeList.end(), TpleComp<edgeListTIds::src>(), comm); 

          //Reserve the approximate required space in our vector
          tupleVector.reserve(edgeList.size());

          for(auto it = edgeList.begin(); it != edgeList.end(); )
          {
            //Range of edges with same source vertex
            auto equalRange = conn::utils::findRange(it, edgeList.end(), *it, TpleComp<edgeListTIds::src>());

            //Range would include atleast 1 element
            assert(std::distance(equalRange.first, equalRange.second) >= 1);

            //Insert the self loop
            tupleVector.emplace_back(std::get<edgeListTIds::src>(*it), MAX_PID, std::get<edgeListTIds::src>(*it)); 

            //Insert other vertex members in this partition 
            for(auto it2 = equalRange.first; it2 != equalRange.second; it2++)
              tupleVector.emplace_back(std::get<edgeListTIds::src>(*it2), MAX_PID, std::get<edgeListTIds::dst>(*it2));;

            it = equalRange.second;
          }

          timer.end_section("vector of tuples initialized for ccl");

          //Log the total count of tuples 
          auto totalTupleCount = mxx::reduce(tupleVector.size(), 0, comm);

          LOG_IF(comm.rank() == 0, INFO) << "Total tuple count is " << totalTupleCount;
        }

        /**
         * @brief     run the iterative algorithm for ccl
         */
        void runConnectedComponentLabeling()
        {
          //variable to track convergence
          bool converged = false;

          //counting iterations
          int iterCount = 0;

          mxx::section_timer timer(std::cerr, comm);

          //range [tupleVector.begin() -- tupleVector.begin() + distance_begin_mid) marks the set of stable partitions in the vector
          //range [tupleVector.begin() + distance_begin_mid -- tupleVector.end()) denotes the active tuples 
          //Initially all the tuples are active, therefore we set distance_begin_mid to 0
          std::size_t distance_begin_mid = 0;

          while(!converged)
          {

            LOG_IF(comm.rank() == 0, INFO) << "Iteration #" << iterCount + 1;
            mxx::section_timer timer2(std::cerr, comm);

            //Temporary storage for extra tuples needed for doubling
            std::vector<T> parentRequestTupleVector;

            //Define the iterators over tupleVector
            auto begin = tupleVector.begin();
            auto mid = tupleVector.begin() + distance_begin_mid;
            auto end = tupleVector.end();

            //Update Pn layer (Explore neighbors of a node and find potential partition candidates
            updatePn(mid, tupleVector.end());

            timer2.end_section("Pn update done");

            //Update the Pc layer, choose the best candidate
            converged = updatePc(mid, tupleVector.end(),parentRequestTupleVector);

            timer2.end_section("Pc update done");

            //Perform pointer doubling if enabled
            if(DOUBLING)
              doPointerDoubling(distance_begin_mid, parentRequestTupleVector);

            timer2.end_section("Pointer doubling done");

            //IMPORTANT : iterators over tupleVector are invalid and need to be redefined
            //Why? Because vector could undergo reallocation during pointer doubling
            
            begin = tupleVector.begin();
            mid = tupleVector.begin() + distance_begin_mid;
            end = tupleVector.end();

            //parition the dataset into stable and active paritions, if optimization is enabled
            if(!converged && (OPTIMIZATION == opt_level::stable_partition_removed || OPTIMIZATION == opt_level::loadbalanced))
            {
              //use std::partition to move stable tuples to the left
              mid = partitionStableTuples<cclTupleIds::Pn>(mid, end);

              timer2.end_section("Stable partitons placed aside");

              if(OPTIMIZATION == opt_level::loadbalanced)
              {
                mid = mxx::block_decompose_partitions(begin, mid, end, comm);
                //Re distributed the tuples to balance the load across the ranks
              
                timer2.end_section("Load balanced");
              }
            }
            distance_begin_mid = std::distance(begin, mid);

            iterCount ++;
          }

          timer.end_section("Total time consumed during coloring");

          LOG_IF(comm.rank() == 0, INFO) << "Algorithm took " << iterCount << " iterations";
        }

        /**
         * @brief             update the Pn layer by sorting the tuples using node ids
         * @param[in] begin   To iterate over the vector of tuples, marks the range of active tuples 
         * @param[in] end     end iterator
         */
        template <typename Iterator>
        void updatePn(Iterator begin, Iterator end)
        {
          //Sort by nid,Pc
          mxx::sort(begin, end, TpleComp2Layers<cclTupleIds::nId, cclTupleIds::Pc>(), comm); 

          //Resolve last and first bucket's boundary splits
          
          //First, find the element with max node id and min Pc locally 
          //Or in other words, get the min Pc of the last bucket
          auto minPcOfLastBucket = mxx::local_reduce(begin, end, TpleReduce2Layers<cclTupleIds::nId, cclTupleIds::Pc, std::greater, std::less>());

          //Second, do exscan, look for max nodeid and min Pc on previous ranks
          auto prevMinPc = mxx::exscan(minPcOfLastBucket, TpleReduce2Layers<cclTupleIds::nId, cclTupleIds::Pc, std::greater, std::less>(), comm);  

          //We also need to know max Pc of the first bucket on the next rank (to check for stability)
          auto maxPcOfFirstBucket = mxx::local_reduce(begin, end, TpleReduce2Layers<cclTupleIds::nId, cclTupleIds::Pc, std::less, std::greater>());

          //reverse exscan, look for min nodeid and max Pc on forward ranks
          auto nextMaxPc = mxx::exscan(maxPcOfFirstBucket,  TpleReduce2Layers<cclTupleIds::nId, cclTupleIds::Pc, std::less, std::greater>(), comm.reverse()); 

          //Now we can update the Pn layer of all the buckets locally
          for(auto it = begin; it !=  end;)
          {
            //Range of tuples with the same node id
            auto equalRange = conn::utils::findRange(it, end, *it, TpleComp<cclTupleIds::nId>());

            //Range would include atleast 1 element
            assert(std::distance(equalRange.first, equalRange.second) >= 1);

            //Minimum Pc from local bucket
            auto thisBucketsMinPcLocal = mxx::local_reduce(equalRange.first, equalRange.second, TpleReduce<cclTupleIds::Pc>());

            //Maximum Pc from local bucket
            auto thisBucketsMaxPcLocal = mxx::local_reduce(equalRange.first, equalRange.second, TpleReduce<cclTupleIds::Pc, std::greater>());

            //For now, mark global minimum as local
            auto thisBucketsMaxPcGlobal = thisBucketsMaxPcLocal;
            auto thisBucketsMinPcGlobal = thisBucketsMinPcLocal;

            //Treat first, last buckets as special cases
            if(equalRange.first == begin)
            {
              //Use value from previous rank
              thisBucketsMinPcGlobal =  comm.rank() == 0 ? thisBucketsMinPcLocal : TpleReduce2Layers<cclTupleIds::nId, cclTupleIds::Pc, std::greater, std::less>() (prevMinPc, thisBucketsMinPcLocal);
            }

            if(equalRange.second == end)
            {
              //Use value from next rank
              thisBucketsMaxPcGlobal = comm.rank() == comm.size() - 1 ? thisBucketsMaxPcLocal : TpleReduce2Layers<cclTupleIds::nId, cclTupleIds::Pc, std::less, std::greater>() (nextMaxPc, thisBucketsMaxPcLocal);

            }

            //If min Pc < max Pc for this bucket, update Pn or else mark them as stable
            if(TpleComp<cclTupleIds::Pc>()(thisBucketsMinPcGlobal, thisBucketsMaxPcGlobal))
              std::for_each(equalRange.first, equalRange.second, [&](T &e){
                  std::get<cclTupleIds::Pn>(e) = std::get<cclTupleIds::Pc>(thisBucketsMinPcGlobal);
                  });
            else
              std::for_each(equalRange.first, equalRange.second, [&](T &e){
                  std::get<cclTupleIds::Pn>(e) = MAX_PID2;
                  });

            //Advance the loop pointer
            it = equalRange.second;
          }
        }

        /**
         * @brief                             update the Pc layer by choosing min Pn
         * @param[in] begin                   To iterate over the vector of tuples, marks the range of active tuples 
         * @param[in] end                     end iterator
         * @param[in] partitionStableTuples   storate to keep 'parentRequest' tuples for doubling
         * @return                            bool value, true if the algorithm is converged
         */
        template <typename Iterator>
          bool updatePc(Iterator begin, Iterator end, std::vector<T>& parentRequestTupleVector)
          {
            //converged yet
            uint8_t converged = 1;    // 1 means true, we will update it below

            //Sort by Pc, Pn
            mxx::sort(begin, end, TpleComp2Layers<cclTupleIds::Pc, cclTupleIds::Pn>(), comm); 

            //Resolve last bucket's boundary split
            
            //First, find the element with max Pc and min Pn locally 
            //Or in other words, get the min Pn of the last bucket
            auto minPnOfLastBucket = mxx::local_reduce(begin, end, TpleReduce2Layers<cclTupleIds::Pc, cclTupleIds::Pn, std::greater, std::less>());

            //Result of exscan, again look for max Pc and min Pn on previous ranks
            auto prevMinPn = mxx::exscan(minPnOfLastBucket, TpleReduce2Layers<cclTupleIds::Pc, cclTupleIds::Pn, std::greater, std::less>(), comm);  


            //Now we can update the Pc layer of all the buckets locally
            for(auto it = begin; it !=  end;)
            {
              //Range of tuples with the same Pc
              auto equalRange = conn::utils::findRange(it, end, *it, TpleComp<cclTupleIds::Pc>());

              //Range would include atleast 1 element
              assert(std::distance(equalRange.first, equalRange.second) >= 1);

              //Minimum Pn from local bucket
              auto thisBucketsMinPnLocal = mxx::local_reduce(equalRange.first, equalRange.second, TpleReduce<cclTupleIds::Pn>());

              //For now, mark global minimum as local
              auto thisBucketsMinPnGlobal = thisBucketsMinPnLocal;

              //Treat first, last buckets as special cases
              if(equalRange.first == begin)
              {
                //Use value from previous rank
                thisBucketsMinPnGlobal =  comm.rank() == 0 ?  thisBucketsMinPnLocal : 
                  TpleReduce2Layers<cclTupleIds::Pc, cclTupleIds::Pn, std::greater, std::less>() (prevMinPn, thisBucketsMinPnLocal);
              }

              //If min Pn < MAX_PID2 for this bucket, update the Pc to new value or else mark the partition as stable
              if(std::get<cclTupleIds::Pn>(thisBucketsMinPnGlobal) < MAX_PID2) 
              {

                //Algorithm not converged yet because we found an active partition
                converged = 0;

                //Update Pc
                std::for_each(equalRange.first, equalRange.second, [&](T &e){
                    std::get<cclTupleIds::Pc>(e) = std::get<cclTupleIds::Pn>(thisBucketsMinPnGlobal);
                    });

                //Insert a 'parentRequest' tuple in the vector for doubling
                if(DOUBLING)
                  parentRequestTupleVector.emplace_back(MAX_PID, MAX_PID, std::get<cclTupleIds::Pn>(thisBucketsMinPnGlobal));
              }
              else
              {
                //stable
                std::for_each(equalRange.first, equalRange.second, [&](T &e){
                    std::get<cclTupleIds::Pn>(e) = MAX_PID;
                    });
              }

              //Advance the loop pointer
              it = equalRange.second;
            }

            //Know convergence of all the ranks
            uint8_t allConverged;
            mxx::allreduce(&converged, 1, &allConverged, mxx::min<uint8_t>(), comm);

            return (allConverged == 1  ? true : false);
          }

        /**
         * @brief                               Function that performs the pointer doubling
         *
         * @details                             
         *                                      'parentRequest' tuples serve the purpose of fetching parent of a partition
         *                                      In the beginning, they have the format <MAX_PID, MAX_PID, newPc>
         *                                      Goal is to 
         *                                        1.  Update the Pn layer of this tuple with the partition id of node newPc
         *                                            Since there could be multiple partition ids of a node 
         *                                            (because a node can belong to multiple partitions at an instant),
         *                                            we pick the minimum of them
         *                                            This would require cut-pasting all the tuples from parentRequestTupleVector to tupleVector
         *                                        2.  Flip the 'parentRequest' tuples and update the partition newPc to the value obtained above
         *                                        3.  Make sure to delete the 'parentRequest' tuples from tupleVector
         *
         * @param[in] beginOffset               tupleVector.begin() + beginOffset would denote the begin iterator for active tuples
         * @param[in] parentRequestTupleVector  All the 'parentRequest' tuples
         */
        void doPointerDoubling(std::size_t beginOffset, std::vector<T>& parentRequestTupleVector)
          {
            //Copy the tuples from parentRequestTupleVector to tupleVector 
            tupleVector.insert(tupleVector.end(), parentRequestTupleVector.begin(), parentRequestTupleVector.end());

            //Range of active tuples in tupleVector needs to be updated 
            auto begin = tupleVector.begin() + beginOffset;
            auto end = tupleVector.end();

            //1. Repeat the procedure of updatePn, but just modify the 'parentRequest' tuples
            //   We can distinguish the 'parentRequest' tuples as they have Pc = MAX_PID

            //Same code as updatePn()
            mxx::sort(begin, end, TpleComp2Layers<cclTupleIds::nId, cclTupleIds::Pc>(), comm); 
            auto minPcOfLastBucket = mxx::local_reduce(begin, end, TpleReduce2Layers<cclTupleIds::nId, cclTupleIds::Pc, std::greater, std::less>());
            auto prevMinPc = mxx::exscan(minPcOfLastBucket, TpleReduce2Layers<cclTupleIds::nId, cclTupleIds::Pc, std::greater, std::less>(), comm);  
            for(auto it = begin; it !=  end;)
            {
              auto equalRange = conn::utils::findRange(it, end, *it, TpleComp<cclTupleIds::nId>());
              auto thisBucketsMinPcLocal = mxx::local_reduce(equalRange.first, equalRange.second, TpleReduce<cclTupleIds::Pc>());
              auto thisBucketsMinPcGlobal = thisBucketsMinPcLocal;
              if(equalRange.first == begin)
              {
                thisBucketsMinPcGlobal =  comm.rank() == 0 ? thisBucketsMinPcLocal : 
                  TpleReduce2Layers<cclTupleIds::nId, cclTupleIds::Pc, std::greater, std::less>() (prevMinPc, thisBucketsMinPcLocal);
              }

              std::for_each(equalRange.first, equalRange.second, [&](T &e){
                    if(std::get<cclTupleIds::Pc>(e) == MAX_PID)
                    {
                      std::get<cclTupleIds::Pn>(e) = std::get<cclTupleIds::Pc>(thisBucketsMinPcGlobal);

                      //flip this 'parentRequest' tuple
                      std::get<cclTupleIds::Pc>(e) =  std::get<cclTupleIds::nId>(e);
                      std::get<cclTupleIds::nId>(e) =  MAX_NID;
                    }
                  });
              it = equalRange.second;
            }

            //2. Now repeat the procedure of updatePc()
            mxx::sort(begin, end, TpleComp2Layers<cclTupleIds::Pc, cclTupleIds::Pn>(), comm); 
            auto minPnOfLastBucket = mxx::local_reduce(begin, end, TpleReduce2Layers<cclTupleIds::Pc, cclTupleIds::Pn, std::greater, std::less>());
            auto prevMinPn = mxx::exscan(minPnOfLastBucket, TpleReduce2Layers<cclTupleIds::Pc, cclTupleIds::Pn, std::greater, std::less>(), comm);  
            for(auto it = begin; it !=  end;)
            {
              auto equalRange = conn::utils::findRange(it, end, *it, TpleComp<cclTupleIds::Pc>());
              auto thisBucketsMinPnLocal = mxx::local_reduce(equalRange.first, equalRange.second, TpleReduce<cclTupleIds::Pn>());
              auto thisBucketsMinPnGlobal = thisBucketsMinPnLocal;
              if(equalRange.first == begin)
              {
                thisBucketsMinPnGlobal =  comm.rank() == 0 ?  thisBucketsMinPnLocal : 
                  TpleReduce2Layers<cclTupleIds::Pc, cclTupleIds::Pn, std::greater, std::less>() (prevMinPn, thisBucketsMinPnLocal);

              }
    
              //Here is the code which updates the Pc for pointer jumping
              //Ignore the stable partitions
              if(std::get<cclTupleIds::Pn>(*equalRange.first) != MAX_PID)
                std::for_each(equalRange.first, equalRange.second, [&](T &e){
                    std::get<cclTupleIds::Pc>(e) = std::get<cclTupleIds::Pn>(thisBucketsMinPnGlobal);
                    });

              it = equalRange.second;
            }

            //3.  Now remove the 'parentRequest' tuples from tupleVector
            //    We can distinguish the 'parentRequest' tuples as they have nId = MAX_NID
            
            //operator should be '!=' because we want 'parentRequest' tuples to move towards right for deletion later
            auto mid = partitionStableTuples<cclTupleIds::nId, std::not_equal_to>(begin,end);

            //Erase the 'parentRequest' tuples
            tupleVector.erase(mid, end);
          }

        /*
         * @brief               partition the tuple array
         * @tparam[in]  layer   the layer of tuple used to partition them
         * @tparam[in]  op      binary operator that takes tuple_value and max
         *
         * @details             if op(value, max) is true, element is moved to left half                
         */
        template <uint8_t layer, template<typename> class op = std::equal_to, typename Iterator>
          Iterator partitionStableTuples(Iterator begin, Iterator end)
          {
            //type of the tuple_element at index layer
            using eleType = typename std::tuple_element<layer, T>::type;

            //max value
            auto max = std::numeric_limits<eleType>::max();

            //do the partition
            return std::partition(begin, end, [&](const T &e){
                return op<eleType>()(std::get<layer>(e), max);
                });
          }

        /**
         * @brief     count the components in the graph after ccl (useful for debugging/testing)
         * @note      should be called after computing connected components. 
         */
        void computeComponentCount()
        {
          //Vector should be sorted by Pc
          if(!mxx::is_sorted(tupleVector.begin(), tupleVector.end(), TpleComp<cclTupleIds::Pc>(), comm))
            mxx::sort(tupleVector.begin(), tupleVector.end(), TpleComp<cclTupleIds::Pc>(), comm);

          //Count unique Pc values
          componentCount =  mxx::uniqueCount(tupleVector.begin(), tupleVector.end(),  TpleComp<cclTupleIds::Pc>(), comm);
        }
    };

  }
}

#endif
