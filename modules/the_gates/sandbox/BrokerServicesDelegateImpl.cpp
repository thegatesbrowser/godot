#include "BrokerServicesDelegateImpl.h"

bool BrokerServicesDelegateImpl::ParallelLaunchEnabled() {
    return false;
}

void BrokerServicesDelegateImpl::ParallelLaunchPostTaskAndReplyWithResult(
    const base::Location& /*from_here*/,
    base::OnceCallback<sandbox::CreateTargetResult()> /*task*/,
    base::OnceCallback<void(sandbox::CreateTargetResult)> /*reply*/) {
    // Stub: No parallel launch support
}

void BrokerServicesDelegateImpl::BeforeTargetProcessCreateOnCreationThread(const void* /*trace_id*/) {
    // Stub
}

void BrokerServicesDelegateImpl::AfterTargetProcessCreateOnCreationThread(const void* /*trace_id*/, DWORD /*process_id*/) {
    // Stub
}

void BrokerServicesDelegateImpl::OnCreateThreadActionCreateFailure(DWORD /*last_error*/) {
    // Stub
}

void BrokerServicesDelegateImpl::OnCreateThreadActionDuplicateFailure(DWORD /*last_error*/) {
    // Stub
} 