// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// See the corresponding header file for description of the functions in this
// file.

#include "chrome/installer/util/install_util.h"

#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>

#include "base/file_util.h"
#include "base/logging.h"
#include "base/path_service.h"
#include "base/scoped_ptr.h"
#include "base/string_util.h"
#include "base/values.h"
#include "base/win/registry.h"
#include "base/win/windows_version.h"
#include "chrome/common/json_value_serializer.h"
#include "chrome/installer/util/browser_distribution.h"
#include "chrome/installer/util/google_update_constants.h"
#include "chrome/installer/util/l10n_string_util.h"
#include "chrome/installer/util/master_preferences_constants.h"
#include "chrome/installer/util/util_constants.h"
#include "chrome/installer/util/work_item_list.h"

using base::win::RegKey;
using installer_util::MasterPreferences;

bool InstallUtil::ExecuteExeAsAdmin(const CommandLine& cmd, DWORD* exit_code) {
  FilePath::StringType program(cmd.GetProgram().value());
  DCHECK(!program.empty());
  DCHECK(program[0] != '\"');

  CommandLine::StringType params(cmd.command_line_string());
  if (params[0] == '"') {
    DCHECK_EQ('"', params[program.length() + 1]);
    DCHECK_EQ(program, params.substr(1, program.length()));
    params = params.substr(program.length() + 2);
  } else {
    DCHECK_EQ(program, params.substr(0, program.length()));
    params = params.substr(program.length());
  }

  TrimWhitespace(params, TRIM_ALL, &params);

  SHELLEXECUTEINFO info = {0};
  info.cbSize = sizeof(SHELLEXECUTEINFO);
  info.fMask = SEE_MASK_NOCLOSEPROCESS;
  info.lpVerb = L"runas";
  info.lpFile = program.c_str();
  info.lpParameters = params.c_str();
  info.nShow = SW_SHOW;
  if (::ShellExecuteEx(&info) == FALSE)
    return false;

  ::WaitForSingleObject(info.hProcess, INFINITE);
  DWORD ret_val = 0;
  if (!::GetExitCodeProcess(info.hProcess, &ret_val))
    return false;

  if (exit_code)
    *exit_code = ret_val;
  return true;
}

std::wstring InstallUtil::GetChromeUninstallCmd(bool system_install,
                                                BrowserDistribution* dist) {
  DCHECK(dist);
  HKEY root = system_install ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
  RegKey key(root, dist->GetUninstallRegPath().c_str(), KEY_READ);
  std::wstring uninstall_cmd;
  key.ReadValue(installer_util::kUninstallStringField, &uninstall_cmd);
  return uninstall_cmd;
}

installer::Version* InstallUtil::GetChromeVersion(BrowserDistribution* dist,
                                                  bool system_install) {
  DCHECK(dist);
  RegKey key;
  std::wstring version_str;

  HKEY reg_root = (system_install) ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
  if (!key.Open(reg_root, dist->GetVersionKey().c_str(), KEY_READ) ||
      !key.ReadValue(google_update::kRegVersionField, &version_str)) {
    VLOG(1) << "No existing Chrome install found.";
    key.Close();
    return NULL;
  }
  key.Close();
  VLOG(1) << "Existing Chrome version found " << version_str;
  return installer::Version::GetVersionFromString(version_str);
}

bool InstallUtil::IsOSSupported() {
  int major, minor;
  base::win::Version version = base::win::GetVersion();
  base::win::GetServicePackLevel(&major, &minor);

  // We do not support Win2K or older, or XP without service pack 2.
  VLOG(1) << "Windows Version: " << version
          << ", Service Pack: " << major << "." << minor;
  return (version > base::win::VERSION_XP) ||
      (version == base::win::VERSION_XP && major >= 2);
}

bool InstallUtil::IsPerUserInstall(const wchar_t* const exe_path) {
  wchar_t program_files_path[MAX_PATH] = {0};
  if (SUCCEEDED(SHGetFolderPath(NULL, CSIDL_PROGRAM_FILES, NULL,
                                SHGFP_TYPE_CURRENT, program_files_path))) {
    return !StartsWith(exe_path, program_files_path, false);
  } else {
    NOTREACHED();
  }
  return true;
}

bool InstallUtil::IsChromeFrameProcess() {
  const MasterPreferences& prefs =
      installer_util::MasterPreferences::ForCurrentProcess();
  return prefs.install_chrome_frame();
}

bool CheckIsChromeSxSProcess() {
  CommandLine* command_line = CommandLine::ForCurrentProcess();
  CHECK(command_line);

  if (command_line->HasSwitch(installer_util::switches::kChromeSxS))
    return true;

  // Also return true if we are running from Chrome SxS installed path.
  FilePath exe_dir;
  PathService::Get(base::DIR_EXE, &exe_dir);
  std::wstring chrome_sxs_dir(installer_util::kGoogleChromeInstallSubDir2);
  chrome_sxs_dir.append(installer_util::kSxSSuffix);
  return FilePath::CompareEqualIgnoreCase(exe_dir.BaseName().value(),
                                          installer_util::kInstallBinaryDir) &&
         FilePath::CompareEqualIgnoreCase(exe_dir.DirName().BaseName().value(),
                                          chrome_sxs_dir);
}

bool InstallUtil::IsChromeSxSProcess() {
  static bool sxs = CheckIsChromeSxSProcess();
  return sxs;
}

bool InstallUtil::BuildDLLRegistrationList(const std::wstring& install_path,
                                           const wchar_t** const dll_names,
                                           int dll_names_count,
                                           bool do_register,
                                           bool user_level_registration,
                                           WorkItemList* registration_list) {
  DCHECK(NULL != registration_list);
  bool success = true;
  for (int i = 0; i < dll_names_count; i++) {
    std::wstring dll_file_path(install_path);
    file_util::AppendToPath(&dll_file_path, dll_names[i]);
    success = registration_list->AddSelfRegWorkItem(dll_file_path,
        do_register, user_level_registration) && success;
  }
  return (dll_names_count > 0) && success;
}

// This method tries to delete a registry key and logs an error message
// in case of failure. It returns true if deletion is successful,
// otherwise false.
bool InstallUtil::DeleteRegistryKey(RegKey& root_key,
                                    const std::wstring& key_path) {
  VLOG(1) << "Deleting registry key " << key_path;
  if (!root_key.DeleteKey(key_path.c_str()) &&
      ::GetLastError() != ERROR_FILE_NOT_FOUND) {
    PLOG(ERROR) << "Failed to delete registry key: " << key_path;
    return false;
  }
  return true;
}

// This method tries to delete a registry value and logs an error message
// in case of failure. It returns true if deletion is successful,
// otherwise false.
bool InstallUtil::DeleteRegistryValue(HKEY reg_root,
                                      const std::wstring& key_path,
                                      const std::wstring& value_name) {
  RegKey key(reg_root, key_path.c_str(), KEY_ALL_ACCESS);
  VLOG(1) << "Deleting registry value " << value_name;
  if (key.ValueExists(value_name.c_str()) &&
      !key.DeleteValue(value_name.c_str())) {
    LOG(ERROR) << "Failed to delete registry value: " << value_name;
    return false;
  }
  return true;
}
