/*=============================================================================

 Library: CppMicroServices

 Copyright (c) The CppMicroServices developers. See the COPYRIGHT
 file at the top-level directory of this distribution and at
 https://github.com/CppMicroServices/CppMicroServices/COPYRIGHT .

 Licensed under the Apache License, Version 2.0 (the "License");
 you may not use this file except in compliance with the License.
 You may obtain a copy of the License at

 http://www.apache.org/licenses/LICENSE-2.0

 Unless required by applicable law or agreed to in writing, software
 distributed under the License is distributed on an "AS IS" BASIS,
 WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 See the License for the specific language governing permissions and
 limitations under the License.

 =============================================================================*/
#include <cassert>
#include "cppmicroservices/ServiceReference.h"
#include "cppmicroservices/LDAPProp.h"
#include "ServiceComponent/ComponentConstants.hpp"
#include "ReferenceManagerImpl.hpp"

using cppmicroservices::logservice::SeverityLevel;
using cppmicroservices::service::component::ComponentConstants::REFERENCE_SCOPE_PROTOTYPE_REQUIRED;
using cppmicroservices::Constants::SERVICE_SCOPE;
using cppmicroservices::Constants::SCOPE_PROTOTYPE;

namespace cppmicroservices {
  namespace scrimpl {

    /**
     * @brief Returns the LDAPFilter of the reference metadata
     * @param refMetadata The metadata representing a service reference
     * @returns a LDAPFilter object corresponding to the @p refMetadata
     */
    LDAPFilter GetReferenceLDAPFilter(const metadata::ReferenceMetadata& refMetadata)
    {
      LDAPPropExpr expr;
      expr = (LDAPProp(cppmicroservices::Constants::OBJECTCLASS) == refMetadata.interfaceName);
      if(!refMetadata.target.empty())
      {
        expr &= LDAPPropExpr(refMetadata.target);
      }

      if(refMetadata.scope == REFERENCE_SCOPE_PROTOTYPE_REQUIRED)
      {
        expr &= (LDAPProp(SERVICE_SCOPE) ==  SCOPE_PROTOTYPE);
      }
      return LDAPFilter(expr);
    }

    ReferenceManagerImpl::ReferenceManagerImpl(const metadata::ReferenceMetadata& metadata,
                                               const cppmicroservices::BundleContext& bc,
                                               std::shared_ptr<cppmicroservices::logservice::LogService> logger)
    : 
     metadata(metadata)
    , tracker(nullptr)
    , logger(std::move(logger))
    {
      if(!bc || !this->logger)
      {
        throw std::invalid_argument("Failed to create object, Invalid arguments passed to constructor");
      }
      try
      {
        tracker = std::make_unique<ServiceTracker<void>>(bc, GetReferenceLDAPFilter(metadata), this);
        tracker->Open();
      }
      catch(...)
      {
        logger->Log(SeverityLevel::LOG_ERROR, "could not open service tracker for " + metadata.interfaceName, std::current_exception());
        tracker.reset();
        throw std::current_exception();
      }
    }

    void ReferenceManagerImpl::WaitForAsyncTask()
    {
      try
      {
        auto asyncTaskHandle = asyncTask.lock();
        if(asyncTaskHandle->valid())
        {
          asyncTaskHandle->get();
        }
      }
      catch (...)
      {
        logger->Log(SeverityLevel::LOG_ERROR, "Exception from async task to manage bound references for " + metadata.interfaceName, std::current_exception());
      }
    }

    void ReferenceManagerImpl::StopTracking()
    {
      try
      {
        tracker->Close();
      }
      catch(...)
      {
        logger->Log(SeverityLevel::LOG_ERROR, "Exception caught while closing service tracker for " + metadata.interfaceName, std::current_exception());
      }
      WaitForAsyncTask();
    }

    std::set<cppmicroservices::ServiceReferenceBase> ReferenceManagerImpl::GetBoundReferences() const
    {
      auto boundRefsHandle = boundRefs.lock();
      return std::set<cppmicroservices::ServiceReferenceBase>(boundRefsHandle->begin(), boundRefsHandle->end());
    }

    std::set<cppmicroservices::ServiceReferenceBase> ReferenceManagerImpl::GetTargetReferences() const
    {
      auto matchedRefsHandle = matchedRefs.lock();
      return std::set<cppmicroservices::ServiceReferenceBase>(matchedRefsHandle->begin(), matchedRefsHandle->end());
    }

    // util method to extract service-id from a given reference
    long GetServiceId(const ServiceReferenceBase& sRef)
    {
      auto idAny = sRef.GetProperty(cppmicroservices::Constants::SERVICE_ID);
      return cppmicroservices::any_cast<long>(idAny);
    }

    bool ReferenceManagerImpl::IsOptional() const
    {
      return (metadata.minCardinality == 0);
    }

    bool ReferenceManagerImpl::IsSatisfied() const
    {
      return (boundRefs.lock()->size() >= metadata.minCardinality);
    }

    ReferenceManagerImpl::~ReferenceManagerImpl()
    {
      StopTracking();
    }

    struct dummyRefObj {
    };

    bool ReferenceManagerImpl::UpdateBoundRefs()
    {
      auto matchedRefsHandle = matchedRefs.lock(); // acquires lock on matchedRefs
      const auto matchedRefsHandleSize = matchedRefsHandle->size();
      if(matchedRefsHandleSize >= metadata.minCardinality)
      {
        auto boundRefsHandle = boundRefs.lock(); // acquires lock on boundRefs
        std::copy_n(matchedRefsHandle->rbegin(),
                    std::min(metadata.maxCardinality, matchedRefsHandleSize),
                    std::inserter(*(boundRefsHandle),
                                  boundRefsHandle->begin()));
        return true;
      }
      return false;
      // release locks on matchedRefs and boundRefs
    }

    // This method implements the following algorithm
    //
    //  if reference becomes satisfied
    //    Copy service references from #matchedRefs to #boundRefs
    //    send a SATISFIED notification to listeners
    //  else if reference is already satisfied
    //    if policyOption is reluctant
    //      ignore the new servcie
    //    else if policyOption is GREEDY
    //      if the new service is better than any of the existing services in #boundRefs
    //        send UNSATISFIED notification to listeners
    //        clear #boundRefs
    //        copy #matchedRefs to #boundRefs
    //        send a SATISFIED notification to listeners
    //      endif
    //    endif
    //  endif
    void ReferenceManagerImpl::ServiceAdded(cppmicroservices::ServiceReferenceBase reference)
    {
      if(!reference)
      {
        logger->Log(SeverityLevel::LOG_DEBUG, "ServiceAdded: service with id " + std::to_string(GetServiceId(reference)) + " has already been unregistered, no-op");
        return;
      }
      const auto minCardinality = metadata.minCardinality;
      const auto maxCardinality = metadata.maxCardinality;
      auto prevSatisfied = false;
      auto becomesSatisfied = false;
      auto replacementNeeded = false;
      auto notifySatisfied = false;
      auto serviceIdToUnbind = -1;

      if(!IsSatisfied())
      {
        notifySatisfied = UpdateBoundRefs(); // becomes satisfied if return value is true
      }
      else // previously satisfied
      {
        if (metadata.policyOption == "greedy")
        {
          auto boundRefsHandle = boundRefs.lock(); // acquire lock on boundRefs
          if (boundRefsHandle->find(reference) == boundRefsHandle->end()) // reference is not bound yet
          {
            if (!boundRefsHandle->empty())
            {
              ServiceReferenceBase minBound = *(boundRefsHandle->begin());
              if (minBound < reference)
              {
                replacementNeeded = true;
                serviceIdToUnbind = GetServiceId(minBound);
              }
            }
            else
            {
              replacementNeeded = IsOptional();
            }
          }
        }
      }

      if(replacementNeeded)
      {
        logger->Log(SeverityLevel::LOG_DEBUG, "Notify UNSATISFIED for reference " + metadata.name);
        RefChangeNotification notification{metadata.name, RefEvent::BECAME_UNSATISFIED};
        NotifyAllListeners(notification); // no lock held during notification

        // The following "clear and copy" strategy is sufficient for
        // updating the boundRefs for static binding policy
        if(0 < serviceIdToUnbind)
        {
          auto boundRefsHandle = boundRefs.lock();
          boundRefsHandle->clear();
        }
        notifySatisfied = UpdateBoundRefs();
      }
      if(notifySatisfied)
      {
        logger->Log(SeverityLevel::LOG_DEBUG, "Notify SATISFIED for reference " + metadata.name);
        RefChangeNotification notification{metadata.name, RefEvent::BECAME_SATISFIED};
        NotifyAllListeners(notification); // no lock held during notifications
      }
    }

    void ReferenceManagerImpl::Enqueue(std::packaged_task<void(void)> task)
    {
      auto drainerTask = [this]{
        std::packaged_task<void(void)> aTask;
        bool queueEmpty = true;
        do
        {
          { // acquire lock on taskQue
            auto taskQueHandle = taskQue.lock();
            if(taskQueHandle->empty())
            {
              break;
            }
            aTask = std::move(taskQueHandle->front());
            taskQueHandle->pop();
            queueEmpty = taskQueHandle->empty();
          } // release lock on taskQue
          try
          {
            aTask(); // no lock held while processing the task, so other threads can add/remove tasks
          }
          catch (...)
          {
            logger->Log(cppmicroservices::logservice::SeverityLevel::LOG_ERROR, "Exception while processing a task to update boundRefs", std::current_exception());
          }
        } while(!queueEmpty); // continue popping tasks until queue is empty
      };

      bool queueWasEmpty = false;

      { // acquire lock on taskQue
        auto taskQueHandle = taskQue.lock();
        queueWasEmpty = taskQueHandle->empty();
        taskQueHandle->push(std::move(task));
      } // release lock on taskQue

      if(queueWasEmpty)
      { // acquire lock on asyncTask
        auto asyncTaskHandle = asyncTask.lock();
        if(asyncTaskHandle->valid())
        {
          // always make sure the previous task has finished.
          asyncTaskHandle->get();
        }
        try
        {
          auto fut = std::async(std::launch::async, drainerTask);
          std::swap(*asyncTaskHandle, fut);
        }
        catch (const std::exception&)
        {
          logger->Log(cppmicroservices::logservice::SeverityLevel::LOG_ERROR, "Failed to spawn an async task", std::current_exception());
        }
      } // release lock on asyncTaskMutex
    }

    cppmicroservices::InterfaceMapConstPtr ReferenceManagerImpl::AddingService(const cppmicroservices::ServiceReference<void>& reference)
    {
      { // acquire lock on matchedRefs
        auto matchedRefsHandle = matchedRefs.lock();
        matchedRefsHandle->insert(reference);
      } // release lock on matchedRefs

      // updating the boundRefs member and notifying listeners happens on a separate thread
      // see "synchronous" section in https://osgi.org/download/r6/osgi.core-6.0.0.pdf#page=432
      // Daisy chain the task to ensure previous request is finished before starting a new one

      ServiceReferenceBase ref(reference);
      std::packaged_task<void(void)> task([this, ref](){
        this->ServiceAdded(ref);
      });
      Enqueue(std::move(task));

      // A non-null object must be returned to indicate to the ServiceTracker that
      // we are tracking the service and need to be called back when the service is removed.
      return MakeInterfaceMap<dummyRefObj>(std::make_shared<dummyRefObj>());
    }

    void ReferenceManagerImpl::ModifiedService(const cppmicroservices::ServiceReference<void>& /*reference*/,
                                               const cppmicroservices::InterfaceMapConstPtr& /*service*/)
    {
      // no-op since there is no use case for property update
    }

    /**
     *This method implements the following algorithm
     *
     * If the removed service is found in the #boundRefs
     *   send a UNSATISFIED notification to listeners
     *   clear the #boundRefs member
     *   copy #matchedRefs to #boundRefs
     *   if reference is still satisfied
     *     send a SATISFIED notification to listeners
     *  endif
     * endif
     */
    void ReferenceManagerImpl::ServiceRemoved(cppmicroservices::ServiceReferenceBase reference)
    {
      auto removedServiceId = GetServiceId(reference);
      auto removeBoundRef = false;

      { // acquire lock on boundRefs
        auto boundRefsHandle = boundRefs.lock();
        auto itr = std::find_if(boundRefsHandle->begin(),
                                boundRefsHandle->end(),
                                [removedServiceId](const ServiceReferenceBase& ref) {
                                  return GetServiceId(ref) == removedServiceId;
                                });
        removeBoundRef = (itr != boundRefsHandle->end());
      } // end lock on boundRefs

      if(removeBoundRef)
      {
        logger->Log(SeverityLevel::LOG_DEBUG, "Notify UNSATISFIED for reference " + metadata.name);
        RefChangeNotification notification { metadata.name, RefEvent::BECAME_UNSATISFIED };
        NotifyAllListeners(notification); // no locks held while notifying listeners

        {
          auto boundRefsHandle = boundRefs.lock();
          boundRefsHandle->clear();
        }
        auto notifySatisfied = UpdateBoundRefs();
        if(notifySatisfied)
        {
          logger->Log(SeverityLevel::LOG_DEBUG, "Notify SATISFIED for reference " + metadata.name);
          RefChangeNotification notification{metadata.name, RefEvent::BECAME_SATISFIED};
          NotifyAllListeners(notification); // no locks held while notifying listeners
        }
      }
    }

    /**
     * If a target service is available to replace the bound service which became unavailable,
     * the component configuration must be reactivated and the replacement service is bound to
     * the new component instance.
     */
    void ReferenceManagerImpl::RemovedService(const cppmicroservices::ServiceReference<void>& reference,
                                              const cppmicroservices::InterfaceMapConstPtr& /*service*/)
    {
      { // acquire lock on matchedRefs
        auto matchedRefsHandle = matchedRefs.lock();
        matchedRefsHandle->erase(reference);
      } // release lock on matchedRefs

      // updating the boundRefs member and notifying listeners happens on a separate thread
      // see "synchronous" section in https://osgi.org/download/r6/osgi.core-6.0.0.pdf#page=432
      // Daisy chain the task to ensure previous request is finished before starting a new one
      ServiceReferenceBase ref(reference);
      std::packaged_task<void(void)> task([this, ref](){
        this->ServiceRemoved(ref);
      });
      Enqueue(std::move(task));
    }

    std::atomic<cppmicroservices::ListenerTokenId> ReferenceManagerImpl::tokenCounter(0);

    /**
     * Method is used to register a listener for callbacks
     */
    cppmicroservices::ListenerTokenId ReferenceManagerImpl::RegisterListener(std::function<void(const RefChangeNotification&)> notify)
    {
      auto notifySatisfied = UpdateBoundRefs();
      if(notifySatisfied)
      {
        RefChangeNotification notification { metadata.name, RefEvent::BECAME_SATISFIED };
        notify(notification);
      }

      cppmicroservices::ListenerTokenId retToken = ++tokenCounter;
      {
        auto listenerMapHandle = listenersMap.lock();
        listenerMapHandle->emplace(retToken, notify);
      }
      return retToken;
    }

    /**
     * Method is used to remove a registered listener
     */
    void ReferenceManagerImpl::UnregisterListener(cppmicroservices::ListenerTokenId token)
    {
      auto listenerMapHandle = listenersMap.lock();
      listenerMapHandle->erase(token);
    }

    /**
     * Method used to notify all listeners
     */
    void ReferenceManagerImpl::NotifyAllListeners(const RefChangeNotification& notification) const noexcept
    {
      RefMgrListenerMap listenersMapCopy;
      {
        auto listenerMapHandle = listenersMap.lock();
        listenersMapCopy = *listenerMapHandle; // copy the listeners map
      }
      for(auto& listenerPair : listenersMapCopy)
      {
        listenerPair.second(notification);
      }
    }
  }
}
