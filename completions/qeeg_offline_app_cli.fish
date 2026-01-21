# Fish completion for qeeg_offline_app_cli
#
# Provides:
#  - completion for offline-app options
#  - dynamic completion of tool names via `qeeg_offline_app_cli --list-tools`
#  - completion of tool names as the first positional argument and after --tool

function __qeeg_offline_app_cli_tools
    qeeg_offline_app_cli --list-tools 2>/dev/null
end

# Options
complete -c qeeg_offline_app_cli -l help -d "Show help"
complete -c qeeg_offline_app_cli -l list-tools -d "List available embedded tools"
complete -c qeeg_offline_app_cli -l json -d "Output JSON"
complete -c qeeg_offline_app_cli -l pretty -d "Pretty-print JSON output"
complete -c qeeg_offline_app_cli -l install-shims -d "Install shim scripts for embedded tools"
complete -c qeeg_offline_app_cli -l uninstall-shims -d "Uninstall shim scripts for embedded tools"
complete -c qeeg_offline_app_cli -l force -d "Force install/uninstall of shims"
complete -c qeeg_offline_app_cli -l dry-run -d "Show what would be done without changing anything"

# --tool <TOOL>
complete -c qeeg_offline_app_cli -l tool -r -d "Filter shim install/uninstall to a single tool" -a "(__qeeg_offline_app_cli_tools)"

# First positional argument (tool name)
complete -c qeeg_offline_app_cli -n "__fish_use_subcommand" -a "(__qeeg_offline_app_cli_tools)" -d "Tool"
