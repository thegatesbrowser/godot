#include "sandbox_win.h"
#include "helpers.h"
#include "core/os/os.h"

#include <sandbox/win/src/sandbox.h>
#include <sandbox/win/src/sandbox_factory.h>
#include <base/win/scoped_process_information.h>
#include <iostream>
#include "BrokerServicesDelegateImpl.h"

void CALLBACK on_process_exit(PVOID lpParameter, BOOLEAN TimerOrWaitFired) {
    DWORD exitCode;
    PROCESS_INFORMATION* pi = static_cast<PROCESS_INFORMATION*>(lpParameter);

    if (GetExitCodeProcess(pi->hProcess, &exitCode)) {
        print_line("Child process exited with code: " + itos(exitCode));
    }

    CloseHandle(pi->hThread);
    CloseHandle(pi->hProcess);
    delete pi;
}

void on_spawn_target_complete(base::win::ScopedProcessInformation process_info, DWORD error_code, sandbox::ResultCode result) {
    ERR_FAIL_COND_MSG(result != sandbox::SBOX_ALL_OK, "Sandbox failed to launch with the following result: " + itos(result));
    ERR_FAIL_COND_MSG(error_code != ERROR_SUCCESS, "Failed to spawn target: " + itos(error_code));

    print_line("Successfully launched sandboxed process");
    ResumeThread(process_info.thread_handle());

    HANDLE waitHandle;
    PROCESS_INFORMATION* piCopy = new PROCESS_INFORMATION(process_info.process_handle(), process_info.thread_handle());

    if (!RegisterWaitForSingleObject(&waitHandle, process_info.process_handle(), on_process_exit, piCopy, INFINITE, WT_EXECUTEONLYONCE)) {
        ERR_PRINT("Failed to register wait callback");
        CloseHandle(process_info.process_handle());
        CloseHandle(process_info.thread_handle());
        delete piCopy;
    }
}

Error SandboxingWin::spawn_target(const Vector<String> &p_arguments) {
	ERR_FAIL_COND_V_MSG(is_target(), ERR_UNAUTHORIZED, "Target process cannot spawn another targets");

    std::unique_ptr<BrokerServicesDelegateImpl> delegate = std::make_unique<BrokerServicesDelegateImpl>();
    sandbox::ResultCode ret = broker_service->Init(std::move(delegate));
	ERR_FAIL_COND_V_MSG(ret != sandbox::SBOX_ALL_OK, FAILED, "Failed to initialize the BrokerServices object");

    std::unique_ptr<sandbox::TargetPolicy> policy = broker_service->CreatePolicy();
    sandbox::TargetConfig* config = policy->GetConfig();

    ret = config->SetJobLevel(sandbox::JobLevel::kLockdown, 0);
	ERR_FAIL_COND_V_MSG(ret != sandbox::SBOX_ALL_OK, FAILED, "Failed to set job level");

    ret = config->SetTokenLevel(sandbox::TokenLevel::USER_RESTRICTED_SAME_ACCESS, sandbox::TokenLevel::USER_LOCKDOWN);
	ERR_FAIL_COND_V_MSG(ret != sandbox::SBOX_ALL_OK, FAILED, "Failed to set token level");

    ret = broker_service->CreateAlternateDesktop(sandbox::Desktop::kAlternateDesktop);
	ERR_FAIL_COND_V_MSG(ret != sandbox::SBOX_ALL_OK, FAILED, "Failed to create alternate desktop");

    config->SetDesktop(sandbox::Desktop::kAlternateDesktop);
    config->SetDelayedIntegrityLevel(sandbox::IntegrityLevel::INTEGRITY_LEVEL_UNTRUSTED);

    // Add additional rules here (ie: file access exceptions) like so:
    ret = config->AllowFileAccess(sandbox::FileSemantics::kAllowAny, L"C:\\Users\\Nordup\\Documents\\Projects\\thegates\\godot\\bin\\sandbox_log.txt");
	ERR_FAIL_COND_V_MSG(ret != sandbox::SBOX_ALL_OK, FAILED, "Failed to set file access");

    String exe_path = OS::get_singleton()->get_executable_path();
    String args_str = String(" ").join(p_arguments);
    broker_service->SpawnTargetAsync(to_wchar(exe_path.utf8().get_data()), to_wchar(args_str.utf8().get_data()), std::move(policy), base::BindOnce(&on_spawn_target_complete));

    return OK;
}

Error test_sandbox() {
    FILE* file = nullptr;
    errno_t err = fopen_s(&file, "C:\\Users\\Nordup\\Documents\\Projects\\thegates\\godot\\bin\\test.txt", "w");

    if (err == 0 && file) {
        fprintf(file, "This should be blocked by the sandbox\n");
        fclose(file);

        ERR_PRINT("Successfully wrote to protected file (sandbox failed!)");
        return FAILED;
    }

    print_line("Could not write to protected file (sandbox working!)");
    return OK;
}

Error SandboxingWin::lower_token() {
	ERR_FAIL_COND_V_MSG(!is_target(), ERR_UNAVAILABLE, "Cannot lower token from broker process");

    sandbox::TargetServices* target_service = sandbox::SandboxFactory::GetTargetServices();
    ERR_FAIL_COND_V_MSG(target_service == nullptr, FAILED, "Failed to retrieve target service");

    sandbox::ResultCode ret = target_service->Init();
	ERR_FAIL_COND_V_MSG(ret != sandbox::SBOX_ALL_OK, FAILED, "Failed to initialize target service");

    // Do any "unsafe" initialization code here, sandbox isn't active yet

    target_service->LowerToken(); // This locks down the sandbox

    // Any code executed at this point is now sandboxed!

    Error err = test_sandbox();
    ERR_FAIL_COND_V_MSG(err != OK, err, "Sandbox tests failed");

    print_line("Sandboxing succeeded");
    return OK;
}

void SandboxingWin::_bind_methods() {
	ClassDB::bind_method(D_METHOD("spawn_target", "arguments"), &SandboxingWin::spawn_target);
}

SandboxingWin::SandboxingWin() {
    broker_service = sandbox::SandboxFactory::GetBrokerServices();
}

SandboxingWin::~SandboxingWin() {
}
