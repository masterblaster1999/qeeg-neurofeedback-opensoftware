# Bash completion for qeeg_nf_cli
#
# Provides basic flag completion plus a few value completions for common options.
#
# Notes:
#  - Protocol names are completed dynamically via `qeeg_nf_cli --list-protocols`.
#  - File/dir completion is provided for a small set of path-like flags.

_qeeg_nf_cli_complete() {
  local cur prev
  cur="${COMP_WORDS[COMP_CWORD]}"
  prev="${COMP_WORDS[COMP_CWORD-1]}"

  local cmd="${COMP_WORDS[0]}"

  _qeeg_nf_cli__protocols() {
    "$cmd" --list-protocols-names 2>/dev/null || "$cmd" --list-protocols 2>/dev/null | sed -n 's/^  \([^ ][^ ]*\).*/\1/p'
  }

  case "$prev" in
    --input|--channel-qc|--audio-wav)
      COMPREPLY=( $(compgen -f -- "$cur") )
      return 0
      ;;
    --outdir)
      COMPREPLY=( $(compgen -d -- "$cur") )
      return 0
      ;;
    --protocol|--protocol-help|--protocol-help-json)
      local prots
      prots="$(_qeeg_nf_cli__protocols | tr '\n' ' ')"
      COMPREPLY=( $(compgen -W "$prots" -- "$cur") )
      return 0
      ;;
    --reward-direction)
      COMPREPLY=( $(compgen -W "above below" -- "$cur") )
      return 0
      ;;
    --adapt-mode)
      COMPREPLY=( $(compgen -W "exp quantile" -- "$cur") )
      return 0
      ;;
    --feedback-mode)
      COMPREPLY=( $(compgen -W "binary continuous" -- "$cur") )
      return 0
      ;;
    --osc-mode)
      COMPREPLY=( $(compgen -W "state split bundle" -- "$cur") )
      return 0
      ;;
  esac

  local opts="--help -h --version --version-json --input --fs --outdir --list-channels --list-channels-json --list-bands --list-bands-json --list-metrics --list-metrics-json --print-config-json --dry-run --list-protocols --list-protocols-names --list-protocols-json --protocol --protocol-help --protocol-help-json --protocol-ch --protocol-a --protocol-b --bands --metric --channel-qc --allow-bad-metric-channels --window --update --metric-smooth --nperseg --overlap --log10 --relative --relative-range --baseline --baseline-quantile --threshold --reward-direction --reward-above --reward-below --target-rate --eta --adapt-mode --adapt-interval --adapt-window --adapt-min-samples --rate-window --reward-on-frames --reward-off-frames --threshold-hysteresis --hysteresis --dwell --refractory --feedback-mode --feedback-span --train-block --rest-block --start-rest --no-adaptation --average-reference --notch --notch-q --bandpass --chunk --realtime --speed --export-bandpowers --export-coherence --artifact-gate --artifact-ptp-z --artifact-rms-z --artifact-kurtosis-z --artifact-min-bad-ch --artifact-ignore --export-artifacts --biotrace-ui --export-derived-events --audio-wav --audio-rate --audio-tone --audio-gain --audio-attack --audio-release --osc-host --osc-port --osc-prefix --osc-mode --pac-bins --pac-trim --pac-zero-phase --demo --seconds"

  if [[ "$cur" == -* ]]; then
    COMPREPLY=( $(compgen -W "$opts" -- "$cur") )
    return 0
  fi

  return 0
}

complete -F _qeeg_nf_cli_complete qeeg_nf_cli
