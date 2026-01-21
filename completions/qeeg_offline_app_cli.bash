# Bash completion for qeeg_offline_app_cli
#
# This completion provides:
#   - First argument: tool names (qeeg_*_cli) or offline-app options
#   - --tool <TOOL> completion for shim install/uninstall filtering
#
# It queries "qeeg_offline_app_cli --list-tools" at completion time so it stays
# in sync with the embedded tool set.

_qeeg_offline_app_cli_complete() {
  local cur prev cword
  cur="${COMP_WORDS[COMP_CWORD]}"
  prev="${COMP_WORDS[COMP_CWORD-1]}"
  cword="${COMP_CWORD}"

  local cmd="${COMP_WORDS[0]}"

  _qeeg_offline_app_cli__tools() {
    "$cmd" --list-tools 2>/dev/null
  }

  # Complete tool names after --tool
  if [[ "$prev" == "--tool" ]]; then
    local tools
    tools="$(_qeeg_offline_app_cli__tools | tr '\n' ' ')"
    COMPREPLY=( $(compgen -W "$tools" -- "$cur") )
    return 0
  fi

  local opts="--help --list-tools --json --pretty --install-shims --uninstall-shims --force --tool --dry-run"

  # First argument: either options or tool name.
  if [[ "$cword" -eq 1 ]]; then
    if [[ "$cur" == --* ]]; then
      COMPREPLY=( $(compgen -W "$opts" -- "$cur") )
    else
      local tools
      tools="$(_qeeg_offline_app_cli__tools | tr '\n' ' ')"
      COMPREPLY=( $(compgen -W "$tools" -- "$cur") )
    fi
    return 0
  fi

  # Otherwise, only complete options when typing an option.
  if [[ "$cur" == --* ]]; then
    COMPREPLY=( $(compgen -W "$opts" -- "$cur") )
    return 0
  fi

  return 0
}

complete -F _qeeg_offline_app_cli_complete qeeg_offline_app_cli
