#include "sandbox_win.h"
#include "helpers.h"

#include <sandbox/win/src/sandbox.h>
#include <sandbox/win/src/sandbox_factory.h>
#include <base/win/scoped_process_information.h>
#include <iostream>
#include "BrokerServicesDelegateImpl.h"

int SandboxingWin::run_parent(List<String> args) {
    sandbox::BrokerServices* broker_service = sandbox::SandboxFactory::GetBrokerServices();

    std::unique_ptr<BrokerServicesDelegateImpl> delegate = std::make_unique<BrokerServicesDelegateImpl>();
    if (sandbox::SBOX_ALL_OK != broker_service->Init(std::move(delegate))) {
        std::wcout << L"Failed to initialize the BrokerServices object" << std::endl;
        return 1;
    }

    PROCESS_INFORMATION pi;
    base::win::ScopedProcessInformation target;

    std::unique_ptr<sandbox::TargetPolicy> policy = broker_service->CreatePolicy();
    sandbox::TargetConfig* config = policy->GetConfig();

    sandbox::ResultCode ret = config->SetJobLevel(sandbox::JobLevel::kLockdown, 0);
    if (ret != sandbox::SBOX_ALL_OK) {
        std::wcout << L"Failed to set job level" << std::endl;
        return 1;
    }

    ret = config->SetTokenLevel(sandbox::TokenLevel::USER_RESTRICTED_SAME_ACCESS, sandbox::TokenLevel::USER_LOCKDOWN);
    if (ret != sandbox::SBOX_ALL_OK) {
        std::wcout << L"Failed to set token level" << std::endl;
        return 1;
    }

    ret = broker_service->CreateAlternateDesktop(sandbox::Desktop::kAlternateDesktop);
    if (ret != sandbox::SBOX_ALL_OK) {
        std::wcout << L"Failed to create alternate desktop" << std::endl;
        return 1;
    }

    config->SetDesktop(sandbox::Desktop::kAlternateDesktop);
    config->SetDelayedIntegrityLevel(sandbox::IntegrityLevel::INTEGRITY_LEVEL_UNTRUSTED);

    // Add additional rules here (ie: file access exceptions) like so:
    ret = config->AllowFileAccess(sandbox::FileSemantics::kAllowAny, L"C:\\Users\\Nordup\\Documents\\Projects\\thegates\\godot\\bin\\sandbox_log.txt");
    if (ret != sandbox::SBOX_ALL_OK) {
        std::wcout << L"Failed to set file access" << std::endl;
        return 1;
    }

    if (args.size() > 0) {
        std::wcout << L"Args: " << args.get(0).utf8().get_data() << std::endl;
    } else {
        std::wcout << L"No args" << std::endl;
        return 1;
    }

    DWORD error_code = 0;
    wchar_t* warg = to_wchar(args.get(0).utf8().get_data());
    sandbox::ResultCode result = broker_service->SpawnTarget(warg, to_wchar("sandbox"), std::move(policy), &error_code, &pi);
    if (sandbox::SBOX_ALL_OK != result) {
        std::wcout << L"Sandbox failed to launch with the following result: " << result << std::endl;
        return 2;
    }
    std::wcout << L"Successfully launched sandboxed process" << std::endl;

    delete[] warg;
    target.Set(pi);

    // std::wcout << L"Sleeping for 15 seconds" << std::endl;
    // Sleep(15000);

    ResumeThread(target.thread_handle());

    // Wait for the child process to complete
    WaitForSingleObject(target.process_handle(), INFINITE);

    DWORD exitCode;
    if (GetExitCodeProcess(target.process_handle(), &exitCode)) {
        std::wcout << L"Child process exited with code: " << exitCode << std::endl;
    }

    return 0;
}

void try_doing_something_bad(FILE* logFile) {
    // Try to write to a protected directory
    FILE* file = nullptr;
    errno_t err = fopen_s(&file, "C:\\Users\\Nordup\\Documents\\Projects\\thegates\\godot\\bin\\test.txt", "w");
    if (err == 0 && file) {
        fprintf(file, "This should be blocked by the sandbox\n");
        fclose(file);
        fprintf(logFile, "Successfully wrote to protected directory (sandbox failed!)\n");
    } else {
        fprintf(logFile, "Could not write to protected directory (sandbox working!)\n");
    }
}

int SandboxingWin::run_child() {
    FILE* logFile = nullptr;
    errno_t err = fopen_s(&logFile, "C:\\Users\\Nordup\\Documents\\Projects\\thegates\\godot\\bin\\sandbox_log.txt", "w");
    if (err != 0 || !logFile) {
        return 1;  // Failed to open log file
    }

    sandbox::TargetServices* target_service = sandbox::SandboxFactory::GetTargetServices();

    if (NULL == target_service) {
        fprintf(logFile, "Failed to retrieve target service\n");
        fclose(logFile);
        return 2;
    }

    if (sandbox::SBOX_ALL_OK != target_service->Init()) {
        fprintf(logFile, "Failed to initialize target service\n");
        fclose(logFile);
        return 3;
    }

    // Do any "unsafe" initialization code here, sandbox isn't active yet

    target_service->LowerToken(); // This locks down the sandbox

    // Any code executed at this point is now sandboxed!

    try_doing_something_bad(logFile);

    fprintf(logFile, "Successfully lower token\n");
    fclose(logFile);
    return 0;
}

int SandboxingWin::run(List<String> args) {
    sandbox::BrokerServices* broker_service = sandbox::SandboxFactory::GetBrokerServices();

    // A non-NULL broker_service means that we are not running in the sandbox, 
    // and are therefore the parent process
    if(NULL != broker_service) {
        return run_parent(args);
    } else {
        return run_child();
    }
}

SandboxingWin::SandboxingWin() {
}

SandboxingWin::~SandboxingWin() {
}
