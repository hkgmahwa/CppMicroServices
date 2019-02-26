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

#ifndef __REFERENCEMANAGERIMPL_HPP__
#define __REFERENCEMANAGERIMPL_HPP__

#include <mutex>
#include <future>
#include <queue>

#include "gtest/gtest_prod.h"
#include "cppmicroservices/BundleContext.h"
#include "cppmicroservices/ServiceTracker.h"
#include "ReferenceManager.hpp"
#include "ConcurrencyUtil.hpp"

namespace cppmicroservices {
  namespace scrimpl {
    using RefMgrListenerMap = std::unordered_map<cppmicroservices::ListenerTokenId, std::function<void(const RefChangeNotification&)>>;
    using TaskQueue = std::queue<std::packaged_task<void()>>;
    /**
     * This class is responsible for tracking a service reference (dependency)
     * and based on the policy criteria, notify the listener about the state
     * changes of the reference
     */
    class ReferenceManagerImpl final : public ReferenceManager, public cppmicroservices::ServiceTrackerCustomizer<void>
    {
    public:
     /**
      * Constructor
      *
      * \param metadata - the reference description as specified in the component description
      * \param bc - the {@link BundleContext} of the bundle containing the component
      * \param logger - the logger object used to log information from this class.
      *
      * \throws \c std::runtime_error if \c bc or \c logger is invalid
      */
      ReferenceManagerImpl(const metadata::ReferenceMetadata& metadata,
                           const cppmicroservices::BundleContext& bc,
                           std::shared_ptr<cppmicroservices::logservice::LogService> logger);
      ReferenceManagerImpl(const ReferenceManagerImpl&) = delete;
      ReferenceManagerImpl(ReferenceManagerImpl&&) = delete;
      ReferenceManagerImpl& operator=(const ReferenceManagerImpl&) = delete;
      ReferenceManagerImpl& operator=(ReferenceManagerImpl&&) = delete;
      ~ReferenceManagerImpl() override;

      /**
       * Returns name of the reference as specified in component description
       */
      std::string GetReferenceName() const override { return metadata.name; }

      /**
       * Returns name of the reference as specified in component description
       */
      std::string GetReferenceScope() const override { return metadata.scope; }

      /**
       * Returns \c LDAPString specifying the match criteria for this dependency
       */
      std::string GetLDAPString() const override { return metadata.target; }

      /**
       * Returns \c true if the dependency is satisfied, \c false otherwise
       */
      bool IsSatisfied() const override;

      /**
       * Returns a set containing all {@link ServiceReferenceBase} objects that are
       * bound when the dependency criteria is satisfied
       */
      std::set<cppmicroservices::ServiceReferenceBase> GetBoundReferences() const override;

      /**
       * Returns a set containing all {@link ServiceReferenceBase} objects that match
       * the dependency criteria
       */
      std::set<cppmicroservices::ServiceReferenceBase> GetTargetReferences() const override;

      /**
       * Returns true if the cardinality for this reference is "optional"
       */
      bool IsOptional() const;

      /**
       * Returns reference metadata associated with this reference
       */
      const metadata::ReferenceMetadata& GetMetadata() const { return metadata; }

      /**
       * Implementation of the {@link ServiceTrackerCustomizer#AddingService} method.
       * The matched references and bound references are updated based on the
       * reference policy criteria
       *
       * \return A dummy object is returned from this method to the framework inorder to
       * receive the #RemovedService callback.
       */
      cppmicroservices::InterfaceMapConstPtr AddingService(const cppmicroservices::ServiceReferenceU& reference) override;

      /**
       * Implementation of the {@link ServiceTrackerCustomizer#ModifiedService} method.
       * No-op at this time
       */
      void ModifiedService(const cppmicroservices::ServiceReferenceU& reference,
                           const cppmicroservices::InterfaceMapConstPtr& service) override;

      /**
       * Implementation of the {@link ServiceTrackerCustomizer#RemovedService} method.
       * The matched references and bound references are updated based on the
       * reference policy criteria
       */
      void RemovedService(const cppmicroservices::ServiceReferenceU& reference,
                          const cppmicroservices::InterfaceMapConstPtr& service) override;

      /**
       * Method is used to receive callbacks when the dependency is satisfied
       */
      cppmicroservices::ListenerTokenId RegisterListener(std::function<void(const RefChangeNotification&)> notify) override;

      /**
       * Method is used to remove the callbacks registered using RegisterListener
       */
      void UnregisterListener(cppmicroservices::ListenerTokenId token) override;

      /**
       * Method to stop tracking the reference service
       */
      void StopTracking() override;

    private:

      FRIEND_TEST(ReferenceManagerImplTest, TestConstructor);
      FRIEND_TEST(ReferenceManagerImplTest, TestConcurrentSatisfied);
      FRIEND_TEST(ReferenceManagerImplTest, TestConcurrentUnsatisfied);
      FRIEND_TEST(ReferenceManagerImplTest, TestConcurrentSatisfiedUnsatisfied);
      FRIEND_TEST(ReferenceManagerImplTest, TestListenerCallbacks);
      FRIEND_TEST(ReferenceManagerImplTest, TestIsSatisfied);
      FRIEND_TEST(ReferenceManagerImplTest, TestEnque);

      /**
       * Helper method to copy service references from #matchedRefs to #boundRefs
       * The copy is performed only if matchedRefs has sufficient items to
       * satisfy the reference's cardinality
       *
       * /return true on success, false otherwise.
       */
      bool UpdateBoundRefs();

      /**
       * Helper method called asynchronously from the ServiceTracker#AddedService
       * callback implemented in this class
       *
       * \param reference is the service reference of a newly available service
       *
       * \note This method is not executed simultaneously from multiple threads.
       */
      void ServiceAdded(cppmicroservices::ServiceReferenceBase reference);

      /**
       * Method to remove a cached service reference from the boundRefs member
       *
       * \param reference is the service reference of a service that has been unregistered
       *
       * \note This method is not executed simultaneously from multiple threads.
       */
      void ServiceRemoved(cppmicroservices::ServiceReferenceBase reference);

      /**
       * Method used to send notification to all the listeners. The #dataMutex mutex must
       * not be locked while notifying the listeners.
       */
      void NotifyAllListeners(const RefChangeNotification& notification) const noexcept;

      /**
       * Method to wait until the asynchronous task is done.
       */
      void WaitForAsyncTask();

      /**
       * Method to add tasks to the task queue. The tasks are added to the queue and processed asynchronously.
       *
       * \param task is the task to be added to the queue. If this is the first
       *        task to enter the queue, an async task is spawned to drain the queue
       */
      void Enqueue(std::packaged_task<void(void)> task);

      const metadata::ReferenceMetadata metadata; ///< reference information from the component description
      std::unique_ptr<ServiceTracker<void>> tracker; ///< used to track service availability
      std::shared_ptr<cppmicroservices::logservice::LogService> logger; ///< logger for this runtime

      mutable Guarded<std::set<cppmicroservices::ServiceReferenceBase>> boundRefs; ///< guarded set of bound references
      mutable Guarded<std::set<cppmicroservices::ServiceReferenceBase>> matchedRefs; ///< guarded set of matched references

      mutable Guarded<RefMgrListenerMap> listenersMap; ///< guarded map of listeners
      static std::atomic<cppmicroservices::ListenerTokenId> tokenCounter; ///< used to generate unique tokens for listeners

      Guarded<TaskQueue> taskQue; ///< queue of tasks resulting from the ServiceTracker callbacks

      Guarded<std::future<void>> asyncTask; ///< asynchronous task spawned to process the tasks in #taskQue
    };
  }
}
#endif // __REFERENCEMANAGERIMPL_HPP__

