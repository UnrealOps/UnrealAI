#!/usr/bin/env bash

set -Eeuo pipefail

script_directory="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repository_root="$(cd -- "${script_directory}/../.." && pwd)"
engine_root="${UNREAL_ENGINE_ROOT:-}"

if [[ -z "${engine_root}" ]]; then
  printf '%s\n' 'UNREAL_ENGINE_ROOT must point to an Unreal Engine installation.' >&2
  exit 2
fi

run_uat="${engine_root}/Engine/Build/BatchFiles/RunUAT.sh"
editor_command="${engine_root}/Engine/Binaries/Mac/UnrealEditor-Cmd"

if [[ ! -x "${run_uat}" ]]; then
  printf 'RunUAT.sh was not found or is not executable: %s\n' "${run_uat}" >&2
  exit 2
fi

if [[ ! -x "${editor_command}" ]]; then
  printf 'UnrealEditor-Cmd was not found or is not executable: %s\n' "${editor_command}" >&2
  exit 2
fi

if [[ -n "${UNREAL_CI_OUTPUT_DIR:-}" ]]; then
  output_root="${UNREAL_CI_OUTPUT_DIR}"
  mkdir -p "${output_root}"
else
  output_root="$(mktemp -d "${TMPDIR:-/tmp}/unrealai-ci.XXXXXX")"
fi

package_directory="${output_root}/Package"
host_project_directory="${output_root}/HostProject"
report_directory="${output_root}/AutomationReport"

if [[ -e "${package_directory}" || -e "${host_project_directory}" || -e "${report_directory}" ]]; then
  printf '%s\n' "CI output paths already exist under ${output_root}; use a fresh UNREAL_CI_OUTPUT_DIR." >&2
  exit 2
fi

python3 "${repository_root}/Scripts/ci/validate_plugin.py"

"${run_uat}" BuildPlugin \
  -Plugin="${repository_root}/UnrealAI.uplugin" \
  -Package="${package_directory}" \
  -TargetPlatforms=Mac \
  -Rocket \
  -StrictIncludes

mkdir -p "${host_project_directory}/Plugins"
cp -R "${package_directory}" "${host_project_directory}/Plugins/UnrealAI"
cp "${repository_root}/Tests/HostProject/UnrealAIHost.uproject" "${host_project_directory}/UnrealAIHost.uproject"
mkdir -p "${report_directory}"

"${editor_command}" "${host_project_directory}/UnrealAIHost.uproject" \
  -unattended \
  -nop4 \
  -NullRHI \
  -nosplash \
  -nosound \
  -stdout \
  -FullStdOutLogOutput \
  -ExecCmds="Automation RunTests UnrealAI" \
  -TestExit="Automation Test Queue Empty" \
  -ReportExportPath="${report_directory}"

python3 "${repository_root}/Scripts/ci/check_automation_report.py" "${report_directory}/index.json"
printf 'UnrealAI package and automation report are available under %s\n' "${output_root}"
