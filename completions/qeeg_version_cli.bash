# Bash completion for qeeg_version_cli
#
# Completes the small set of supported flags.

_qeeg_version_cli_complete() {
  local cur
  cur="${COMP_WORDS[COMP_CWORD]}"

  local opts="--help -h --full --json"

  if [[ "$cur" == -* ]]; then
    COMPREPLY=( $(compgen -W "$opts" -- "$cur") )
    return 0
  fi

  return 0
}

complete -F _qeeg_version_cli_complete qeeg_version_cli
