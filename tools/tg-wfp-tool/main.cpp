// tg-wfp-tool.cpp — TheGates WFP install/uninstall/list helper.
//
// Standalone Win32 binary. Reads or modifies the persistent WFP filter set
// scoped to TheGates' renderer AppContainer. The launcher's Inno Setup
// installer invokes this during its elevated phase to install filters at
// install time, and during uninstall to remove them.
//
// Usage:
//   tg-wfp-tool.exe install     — register provider + filters (needs admin)
//   tg-wfp-tool.exe uninstall   — remove provider + filters (needs admin)
//   tg-wfp-tool.exe list        — print filter IDs under our provider
//
// Build (clang-cl):
//   clang-cl /std:c++17 main.cpp /link fwpuclnt.lib rpcrt4.lib /OUT:tg-wfp-tool.exe
//
// Build (MSVC):
//   cl /std:c++17 /EHsc main.cpp /link fwpuclnt.lib rpcrt4.lib /OUT:tg-wfp-tool.exe

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <fwpmu.h>
#include <initguid.h>
#include <rpc.h>
#include <sddl.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#pragma comment(lib, "fwpuclnt.lib")
#pragma comment(lib, "rpcrt4.lib")
#pragma comment(lib, "advapi32.lib")

// Provider GUID — must match the constant in
// modules/the_gates/network/windows/network_filter_windows.cpp.
// {a4f1b8a6-7e8c-4f1c-9a3b-9f1d2c5e1a01}
DEFINE_GUID(TG_WFP_PROVIDER_GUID,
		0xa4f1b8a6, 0x7e8c, 0x4f1c, 0x9a, 0x3b, 0x9f, 0x1d, 0x2c, 0x5e, 0x1a, 0x01);

// Sublayer GUID — our own sublayer keeps weight ordering predictable.
// {a4f1b8a6-7e8c-4f1c-9a3b-9f1d2c5e1a02}
DEFINE_GUID(TG_WFP_SUBLAYER_GUID,
		0xa4f1b8a6, 0x7e8c, 0x4f1c, 0x9a, 0x3b, 0x9f, 0x1d, 0x2c, 0x5e, 0x1a, 0x02);

// Image path that the launcher passes as the renderer binary location.
// Filters use FWPM_CONDITION_ALE_APP_ID to match traffic from this exe (and
// any process it spawns that inherits the same image identity).
// Set by `tg-wfp-tool install <path>`.
static std::wstring g_renderer_image_path;

// CIDR ranges to block — RFC 1918 + loopback + link-local + IPv4-mapped IPv6.
struct CIDR4 {
	UINT32 addr;
	UINT32 mask;
};

static const CIDR4 kBlockV4[] = {
	{ 0x0A000000u, 0xFF000000u }, // 10.0.0.0/8
	{ 0xAC100000u, 0xFFF00000u }, // 172.16.0.0/12
	{ 0xC0A80000u, 0xFFFF0000u }, // 192.168.0.0/16
	{ 0x7F000000u, 0xFF000000u }, // 127.0.0.0/8
	{ 0xA9FE0000u, 0xFFFF0000u }, // 169.254.0.0/16
};

struct CIDR6 {
	UINT8 addr[16];
	UINT8 prefix;
};

static const CIDR6 kBlockV6[] = {
	// ::1/128
	{ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 }, 128 },
	// fc00::/7
	{ { 0xfc, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, 7 },
	// fe80::/10
	{ { 0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, 10 },
	// ::ffff:0:0/96 (IPv4-mapped)
	{ { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff, 0, 0, 0, 0 }, 96 },
};

// FWPM_CONDITION_ALE_APP_ID expects an NT device path blob, not a DOS path.
// FwpmGetAppIdFromFileName0 handles the conversion (resolves \??\C:\... etc.).
static bool resolve_app_id_blob(const wchar_t *dos_path, FWP_BYTE_BLOB **out_blob) {
	const DWORD r = FwpmGetAppIdFromFileName0(dos_path, out_blob);
	if (r != ERROR_SUCCESS) {
		fprintf(stderr, "FwpmGetAppIdFromFileName0 failed for %ls: %lu\n", dos_path, r);
		return false;
	}
	return true;
}

static DWORD add_v4_filter(HANDLE engine, FWP_BYTE_BLOB *app_id, UINT32 addr, UINT32 mask, const wchar_t *name) {
	FWP_V4_ADDR_AND_MASK addr_mask = { addr, mask };

	FWPM_FILTER_CONDITION0 conds[2] = {};
	conds[0].fieldKey = FWPM_CONDITION_ALE_APP_ID;
	conds[0].matchType = FWP_MATCH_EQUAL;
	conds[0].conditionValue.type = FWP_BYTE_BLOB_TYPE;
	conds[0].conditionValue.byteBlob = app_id;

	conds[1].fieldKey = FWPM_CONDITION_IP_REMOTE_ADDRESS;
	conds[1].matchType = FWP_MATCH_EQUAL;
	conds[1].conditionValue.type = FWP_V4_ADDR_MASK;
	conds[1].conditionValue.v4AddrMask = &addr_mask;

	FWPM_FILTER0 f = {};
	f.layerKey = FWPM_LAYER_ALE_AUTH_CONNECT_V4;
	f.subLayerKey = TG_WFP_SUBLAYER_GUID;
	f.action.type = FWP_ACTION_BLOCK;
	f.weight.type = FWP_EMPTY;
	f.numFilterConditions = 2;
	f.filterCondition = conds;
	f.flags = FWPM_FILTER_FLAG_PERSISTENT;
	f.providerKey = (GUID *)&TG_WFP_PROVIDER_GUID;
	f.displayData.name = (wchar_t *)name;

	UINT64 filter_id = 0;
	return FwpmFilterAdd0(engine, &f, nullptr, &filter_id);
}

static DWORD add_v6_filter(HANDLE engine, FWP_BYTE_BLOB *app_id, const UINT8 addr[16], UINT8 prefix, const wchar_t *name) {
	FWP_V6_ADDR_AND_MASK addr_mask = {};
	memcpy(addr_mask.addr, addr, 16);
	addr_mask.prefixLength = prefix;

	FWPM_FILTER_CONDITION0 conds[2] = {};
	conds[0].fieldKey = FWPM_CONDITION_ALE_APP_ID;
	conds[0].matchType = FWP_MATCH_EQUAL;
	conds[0].conditionValue.type = FWP_BYTE_BLOB_TYPE;
	conds[0].conditionValue.byteBlob = app_id;

	conds[1].fieldKey = FWPM_CONDITION_IP_REMOTE_ADDRESS;
	conds[1].matchType = FWP_MATCH_EQUAL;
	conds[1].conditionValue.type = FWP_V6_ADDR_MASK;
	conds[1].conditionValue.v6AddrMask = &addr_mask;

	FWPM_FILTER0 f = {};
	f.layerKey = FWPM_LAYER_ALE_AUTH_CONNECT_V6;
	f.subLayerKey = TG_WFP_SUBLAYER_GUID;
	f.action.type = FWP_ACTION_BLOCK;
	f.weight.type = FWP_EMPTY;
	f.numFilterConditions = 2;
	f.filterCondition = conds;
	f.flags = FWPM_FILTER_FLAG_PERSISTENT;
	f.providerKey = (GUID *)&TG_WFP_PROVIDER_GUID;
	f.displayData.name = (wchar_t *)name;

	UINT64 filter_id = 0;
	return FwpmFilterAdd0(engine, &f, nullptr, &filter_id);
}

static int do_install() {
	HANDLE engine = nullptr;
	DWORD res = FwpmEngineOpen0(nullptr, RPC_C_AUTHN_DEFAULT, nullptr, nullptr, &engine);
	if (res != ERROR_SUCCESS) {
		fprintf(stderr, "FwpmEngineOpen0 failed: %lu (admin required?)\n", res);
		return 2;
	}

	res = FwpmTransactionBegin0(engine, 0);
	if (res != ERROR_SUCCESS) {
		fprintf(stderr, "FwpmTransactionBegin0 failed: %lu\n", res);
		FwpmEngineClose0(engine);
		return 2;
	}

	// Register provider.
	FWPM_PROVIDER0 provider = {};
	provider.providerKey = TG_WFP_PROVIDER_GUID;
	const wchar_t *provider_name = L"TheGates Network Filter";
	const wchar_t *provider_desc = L"Blocks TheGates renderer from RFC 1918 / loopback / link-local destinations.";
	provider.displayData.name = (wchar_t *)provider_name;
	provider.displayData.description = (wchar_t *)provider_desc;
	provider.flags = FWPM_PROVIDER_FLAG_PERSISTENT;
	res = FwpmProviderAdd0(engine, &provider, nullptr);
	if (res != ERROR_SUCCESS && res != FWP_E_ALREADY_EXISTS) {
		fprintf(stderr, "FwpmProviderAdd0 failed: %lu\n", res);
		FwpmTransactionAbort0(engine);
		FwpmEngineClose0(engine);
		return 2;
	}

	// Register sublayer.
	FWPM_SUBLAYER0 sublayer = {};
	sublayer.subLayerKey = TG_WFP_SUBLAYER_GUID;
	const wchar_t *sublayer_name = L"TheGates Sublayer";
	sublayer.displayData.name = (wchar_t *)sublayer_name;
	sublayer.providerKey = (GUID *)&TG_WFP_PROVIDER_GUID;
	sublayer.flags = FWPM_SUBLAYER_FLAG_PERSISTENT;
	// Sublayer weight is UINT16. Max value outbids any user-mode sublayer
	// (including the Windows Defender WSH sublayer at ~0xFFFE) so our
	// explicit blocks beat its blanket outbound permit. WFP layer arbitration:
	// highest-weight sublayer's verdict (block/permit) is final; lower-weight
	// sublayers only run when higher ones return CONTINUE.
	sublayer.weight = 0xFFFF;
	res = FwpmSubLayerAdd0(engine, &sublayer, nullptr);
	if (res != ERROR_SUCCESS && res != FWP_E_ALREADY_EXISTS) {
		fprintf(stderr, "FwpmSubLayerAdd0 failed: %lu\n", res);
		FwpmTransactionAbort0(engine);
		FwpmEngineClose0(engine);
		return 2;
	}

	FWP_BYTE_BLOB *app_id = nullptr;
	if (!resolve_app_id_blob(g_renderer_image_path.c_str(), &app_id)) {
		FwpmTransactionAbort0(engine);
		FwpmEngineClose0(engine);
		return 2;
	}

	// IPv4 block filters.
	int added = 0;
	for (size_t i = 0; i < sizeof(kBlockV4) / sizeof(kBlockV4[0]); i++) {
		wchar_t name[128];
		swprintf_s(name, L"TheGates: block renderer v4 %u", (unsigned)i);
		res = add_v4_filter(engine, app_id, kBlockV4[i].addr, kBlockV4[i].mask, name);
		if (res != ERROR_SUCCESS) {
			fprintf(stderr, "add_v4_filter[%zu] failed: %lu\n", i, res);
			FwpmFreeMemory0((void **)&app_id);
			FwpmTransactionAbort0(engine);
			FwpmEngineClose0(engine);
			return 2;
		}
		added++;
	}

	// IPv6 block filters.
	for (size_t i = 0; i < sizeof(kBlockV6) / sizeof(kBlockV6[0]); i++) {
		wchar_t name[128];
		swprintf_s(name, L"TheGates: block renderer v6 %u", (unsigned)i);
		res = add_v6_filter(engine, app_id, kBlockV6[i].addr, kBlockV6[i].prefix, name);
		if (res != ERROR_SUCCESS) {
			fprintf(stderr, "add_v6_filter[%zu] failed: %lu\n", i, res);
			FwpmFreeMemory0((void **)&app_id);
			FwpmTransactionAbort0(engine);
			FwpmEngineClose0(engine);
			return 2;
		}
		added++;
	}

	FwpmFreeMemory0((void **)&app_id);

	res = FwpmTransactionCommit0(engine);
	if (res != ERROR_SUCCESS) {
		fprintf(stderr, "FwpmTransactionCommit0 failed: %lu\n", res);
		FwpmTransactionAbort0(engine);
		FwpmEngineClose0(engine);
		return 2;
	}

	FwpmEngineClose0(engine);

	// Write the install marker under HKLM. WFP enumeration is denied for
	// non-admin callers, so the user-mode launcher reads this marker instead
	// of querying WFP directly. Admin to write (we are), read for everyone.
	HKEY key = nullptr;
	LSTATUS rs = RegCreateKeyExW(HKEY_LOCAL_MACHINE,
			L"SOFTWARE\\TheGates\\NetworkFilter",
			0, nullptr, REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &key, nullptr);
	if (rs == ERROR_SUCCESS) {
		const DWORD installed = 1;
		const DWORD filter_count = (DWORD)added;
		RegSetValueExW(key, L"Installed", 0, REG_DWORD,
				reinterpret_cast<const BYTE *>(&installed), sizeof(DWORD));
		RegSetValueExW(key, L"FilterCount", 0, REG_DWORD,
				reinterpret_cast<const BYTE *>(&filter_count), sizeof(DWORD));
		RegCloseKey(key);
	}

	printf("Installed %d WFP filters under TheGates provider.\n", added);
	return 0;
}

static int do_uninstall() {
	HANDLE engine = nullptr;
	DWORD res = FwpmEngineOpen0(nullptr, RPC_C_AUTHN_DEFAULT, nullptr, nullptr, &engine);
	if (res != ERROR_SUCCESS) {
		fprintf(stderr, "FwpmEngineOpen0 failed: %lu (admin required?)\n", res);
		return 2;
	}

	// Enumerate and delete filters under our provider.
	int removed = 0;
	for (const GUID layer : { FWPM_LAYER_ALE_AUTH_CONNECT_V4, FWPM_LAYER_ALE_AUTH_CONNECT_V6 }) {
		HANDLE enum_handle = nullptr;
		FWPM_FILTER_ENUM_TEMPLATE0 tmpl = {};
		tmpl.providerKey = (GUID *)&TG_WFP_PROVIDER_GUID;
		tmpl.layerKey = layer;
		tmpl.actionMask = 0xFFFFFFFF;
		res = FwpmFilterCreateEnumHandle0(engine, &tmpl, &enum_handle);
		if (res != ERROR_SUCCESS) {
			continue;
		}
		FWPM_FILTER0 **entries = nullptr;
		UINT32 num = 0;
		res = FwpmFilterEnum0(engine, enum_handle, 256, &entries, &num);
		if (res == ERROR_SUCCESS) {
			for (UINT32 i = 0; i < num; i++) {
				FwpmFilterDeleteById0(engine, entries[i]->filterId);
				removed++;
			}
			FwpmFreeMemory0((void **)&entries);
		}
		FwpmFilterDestroyEnumHandle0(engine, enum_handle);
	}

	(void)FwpmSubLayerDeleteByKey0(engine, &TG_WFP_SUBLAYER_GUID);
	(void)FwpmProviderDeleteByKey0(engine, &TG_WFP_PROVIDER_GUID);

	FwpmEngineClose0(engine);

	// Clear the install marker.
	RegDeleteKeyW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\TheGates\\NetworkFilter");

	printf("Removed %d filters and provider+sublayer.\n", removed);
	return 0;
}

static int do_list() {
	HANDLE engine = nullptr;
	DWORD res = FwpmEngineOpen0(nullptr, RPC_C_AUTHN_DEFAULT, nullptr, nullptr, &engine);
	if (res != ERROR_SUCCESS) {
		fprintf(stderr, "FwpmEngineOpen0 failed: %lu\n", res);
		return 2;
	}
	int total = 0;
	for (const GUID layer : { FWPM_LAYER_ALE_AUTH_CONNECT_V4, FWPM_LAYER_ALE_AUTH_CONNECT_V6 }) {
		HANDLE enum_handle = nullptr;
		FWPM_FILTER_ENUM_TEMPLATE0 tmpl = {};
		tmpl.providerKey = (GUID *)&TG_WFP_PROVIDER_GUID;
		tmpl.layerKey = layer;
		tmpl.actionMask = 0xFFFFFFFF;
		res = FwpmFilterCreateEnumHandle0(engine, &tmpl, &enum_handle);
		if (res != ERROR_SUCCESS) {
			continue;
		}
		FWPM_FILTER0 **entries = nullptr;
		UINT32 num = 0;
		res = FwpmFilterEnum0(engine, enum_handle, 256, &entries, &num);
		if (res == ERROR_SUCCESS) {
			for (UINT32 i = 0; i < num; i++) {
				printf("  filterId=%llu layer=%s name=%ls\n",
						(unsigned long long)entries[i]->filterId,
						(IsEqualGUID(layer, FWPM_LAYER_ALE_AUTH_CONNECT_V4) ? "v4" : "v6"),
						entries[i]->displayData.name ? entries[i]->displayData.name : L"(no name)");
				total++;
			}
			FwpmFreeMemory0((void **)&entries);
		}
		FwpmFilterDestroyEnumHandle0(engine, enum_handle);
	}
	FwpmEngineClose0(engine);
	printf("%d total filters under TheGates provider.\n", total);
	return 0;
}

// Converts a narrow argv path to wide string for the Windows APIs.
static std::wstring widen(const char *p) {
	const int n = MultiByteToWideChar(CP_UTF8, 0, p, -1, nullptr, 0);
	if (n <= 0) {
		return std::wstring();
	}
	std::wstring w(n - 1, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, p, -1, w.data(), n);
	return w;
}

int main(int argc, char *argv[]) {
	if (argc < 2) {
		fprintf(stderr,
				"Usage:\n"
				"  tg-wfp-tool.exe install <renderer-exe-path>\n"
				"  tg-wfp-tool.exe uninstall\n"
				"  tg-wfp-tool.exe list\n");
		return 1;
	}
	if (strcmp(argv[1], "install") == 0) {
		if (argc < 3) {
			fprintf(stderr, "install: missing <renderer-exe-path> argument\n");
			return 1;
		}
		g_renderer_image_path = widen(argv[2]);
		if (g_renderer_image_path.empty()) {
			fprintf(stderr, "install: invalid renderer path\n");
			return 1;
		}
		return do_install();
	}
	if (strcmp(argv[1], "uninstall") == 0) {
		return do_uninstall();
	}
	if (strcmp(argv[1], "list") == 0) {
		return do_list();
	}
	fprintf(stderr, "Unknown command: %s\n", argv[1]);
	return 1;
}
