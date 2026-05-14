#ifndef BROKER_SERVICES_DELEGATE_IMPL_H_
#define BROKER_SERVICES_DELEGATE_IMPL_H_

#include <sandbox/win/src/sandbox.h>

class BrokerServicesDelegateImpl : public sandbox::BrokerServicesDelegate {
 public:
  bool ParallelLaunchEnabled() override;
  void ParallelLaunchPostTaskAndReplyWithResult(
      const base::Location& from_here,
      base::OnceCallback<sandbox::CreateTargetResult()> task,
      base::OnceCallback<void(sandbox::CreateTargetResult)> reply) override;
  void BeforeTargetProcessCreateOnCreationThread(const void* trace_id) override;
  void AfterTargetProcessCreateOnCreationThread(const void* trace_id, DWORD process_id) override;
  void OnCreateThreadActionCreateFailure(DWORD last_error) override;
  void OnCreateThreadActionDuplicateFailure(DWORD last_error) override;
  ~BrokerServicesDelegateImpl() override = default;
};

#endif // BROKER_SERVICES_DELEGATE_IMPL_H_ 