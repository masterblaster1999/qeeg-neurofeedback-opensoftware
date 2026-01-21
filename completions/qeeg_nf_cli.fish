# Fish completion for qeeg_nf_cli
#
# Provides completion for common options and dynamic completion of built-in
# protocol preset names via `qeeg_nf_cli --list-protocols`.

function __qeeg_nf_cli_protocols
    qeeg_nf_cli --list-protocols-names 2>/dev/null; or begin
        qeeg_nf_cli --list-protocols 2>/dev/null | string match -r '^  [^ ]+' | string replace -r '^  ([^ ]+).*' '$1'
    end
end

# Help
complete -c qeeg_nf_cli -s h -l help -d "Show help"
complete -c qeeg_nf_cli -l version -d "Print version and exit"
complete -c qeeg_nf_cli -l version-json -d "Print version/build metadata as JSON and exit"

# Core inputs/outputs
complete -c qeeg_nf_cli -l input -r -d "Input EDF/BDF/CSV" -a "(__fish_complete_path)"
complete -c qeeg_nf_cli -l fs -r -d "Sampling rate for CSV/demo (Hz)"
complete -c qeeg_nf_cli -l outdir -r -d "Output directory" -a "(__fish_complete_directories)"

# Input inspection
complete -c qeeg_nf_cli -l list-channels -d "Print input channel names (one per line) and exit"
complete -c qeeg_nf_cli -l list-channels-json -d "Print input channel info as JSON and exit"
complete -c qeeg_nf_cli -l list-bands -d "Print default EEG bands (name:lo-hi) and exit"
complete -c qeeg_nf_cli -l list-bands-json -d "Print default EEG bands as JSON and exit"
complete -c qeeg_nf_cli -l list-metrics -d "Print metric spec examples and exit"
complete -c qeeg_nf_cli -l list-metrics-json -d "Print metric spec examples as JSON and exit"
complete -c qeeg_nf_cli -l print-config-json -d "Print resolved configuration as JSON and exit"
complete -c qeeg_nf_cli -l dry-run -d "Validate args/inputs and exit (no outputs written)"

# Protocol presets
complete -c qeeg_nf_cli -l list-protocols -d "List built-in protocol presets and exit"
complete -c qeeg_nf_cli -l list-protocols-names -d "List preset names (one per line) and exit"
complete -c qeeg_nf_cli -l list-protocols-json -d "List presets as JSON and exit"
complete -c qeeg_nf_cli -l protocol -r -d "Apply built-in protocol preset" -a "(__qeeg_nf_cli_protocols)"
complete -c qeeg_nf_cli -l protocol-help -r -d "Show protocol preset details and exit" -a "(__qeeg_nf_cli_protocols)"
complete -c qeeg_nf_cli -l protocol-help-json -r -d "Show protocol preset details as JSON and exit" -a "(__qeeg_nf_cli_protocols)"
complete -c qeeg_nf_cli -l protocol-ch -r -d "Override {ch} for single-channel presets"
complete -c qeeg_nf_cli -l protocol-a -r -d "Override {a} for pair presets"
complete -c qeeg_nf_cli -l protocol-b -r -d "Override {b} for pair presets"

# Metric / bands
complete -c qeeg_nf_cli -l bands -r -d "Band spec string"
complete -c qeeg_nf_cli -l metric -r -d "Metric spec string"
complete -c qeeg_nf_cli -l window -r -d "Sliding window seconds"
complete -c qeeg_nf_cli -l update -r -d "Update interval seconds"
complete -c qeeg_nf_cli -l metric-smooth -r -d "EMA metric smooth seconds"
complete -c qeeg_nf_cli -l nperseg -r -d "Welch segment length"
complete -c qeeg_nf_cli -l overlap -r -d "Welch overlap fraction"

# Bandpower scaling
complete -c qeeg_nf_cli -l log10 -d "Use log10(power) for bandpower-like metrics"
complete -c qeeg_nf_cli -l relative -d "Use relative power normalization"
complete -c qeeg_nf_cli -l relative-range -r -d "Relative power range LO HI"

# Threshold / adaptation
complete -c qeeg_nf_cli -l baseline -r -d "Baseline seconds for initial threshold"
complete -c qeeg_nf_cli -l baseline-quantile -r -d "Baseline quantile in [0,1]"
complete -c qeeg_nf_cli -l threshold -r -d "Explicit initial threshold"
complete -c qeeg_nf_cli -l reward-direction -r -d "Reward direction" -a "above below"
complete -c qeeg_nf_cli -l reward-above -d "Alias: reward direction above"
complete -c qeeg_nf_cli -l reward-below -d "Alias: reward direction below"
complete -c qeeg_nf_cli -l target-rate -r -d "Target reward rate"
complete -c qeeg_nf_cli -l eta -r -d "Adaptive threshold gain"
complete -c qeeg_nf_cli -l adapt-mode -r -d "Adaptive threshold mode" -a "exp quantile"
complete -c qeeg_nf_cli -l adapt-interval -r -d "Adapt threshold every S seconds"
complete -c qeeg_nf_cli -l adapt-window -r -d "Quantile mode: rolling window seconds"
complete -c qeeg_nf_cli -l adapt-min-samples -r -d "Quantile mode: minimum samples"
complete -c qeeg_nf_cli -l rate-window -r -d "Reward-rate window seconds"
complete -c qeeg_nf_cli -l no-adaptation -d "Disable adaptive thresholding"

# Debounce / hysteresis / shaping
complete -c qeeg_nf_cli -l reward-on-frames -r -d "Debounce ON frames"
complete -c qeeg_nf_cli -l reward-off-frames -r -d "Debounce OFF frames"
complete -c qeeg_nf_cli -l threshold-hysteresis -r -d "Numeric hysteresis band"
complete -c qeeg_nf_cli -l hysteresis -r -d "Alias for --threshold-hysteresis"
complete -c qeeg_nf_cli -l dwell -r -d "Reward shaping dwell seconds"
complete -c qeeg_nf_cli -l refractory -r -d "Reward shaping refractory seconds"

# Feedback value
complete -c qeeg_nf_cli -l feedback-mode -r -d "Feedback value mode" -a "binary continuous"
complete -c qeeg_nf_cli -l feedback-span -r -d "Continuous mode span"

# Training schedule
complete -c qeeg_nf_cli -l train-block -r -d "Training block seconds"
complete -c qeeg_nf_cli -l rest-block -r -d "Rest block seconds"
complete -c qeeg_nf_cli -l start-rest -d "Start schedule with rest"

# Preprocessing
complete -c qeeg_nf_cli -l average-reference -d "Apply common average reference"
complete -c qeeg_nf_cli -l notch -r -d "Notch frequency (Hz)"
complete -c qeeg_nf_cli -l notch-q -r -d "Notch Q factor"
complete -c qeeg_nf_cli -l bandpass -r -d "Bandpass LO HI (Hz)"

# Playback pacing
complete -c qeeg_nf_cli -l chunk -r -d "Playback chunk seconds"
complete -c qeeg_nf_cli -l realtime -d "Pace playback at 1x real-time"
complete -c qeeg_nf_cli -l speed -r -d "Playback speed multiplier"

# Exports
complete -c qeeg_nf_cli -l export-bandpowers -d "Write bandpower_timeseries.csv"
complete -c qeeg_nf_cli -l export-coherence -d "Write coherence timeseries CSV"
complete -c qeeg_nf_cli -l export-artifacts -d "Write artifact gate timeseries CSV"
complete -c qeeg_nf_cli -l biotrace-ui -d "Write BioTrace+ style HTML UI export"
complete -c qeeg_nf_cli -l export-derived-events -d "Write derived events CSV/TSV/JSON"

# Artifact gating
complete -c qeeg_nf_cli -l artifact-gate -d "Enable artifact gating"
complete -c qeeg_nf_cli -l artifact-ptp-z -r -d "Artifact PTP robust z threshold"
complete -c qeeg_nf_cli -l artifact-rms-z -r -d "Artifact RMS robust z threshold"
complete -c qeeg_nf_cli -l artifact-kurtosis-z -r -d "Artifact kurtosis robust z threshold"
complete -c qeeg_nf_cli -l artifact-min-bad-ch -r -d "Artifact bad if >=N channels flagged"
complete -c qeeg_nf_cli -l artifact-ignore -r -d "Comma-separated channels to ignore"

# Channel QC
complete -c qeeg_nf_cli -l channel-qc -r -d "Channel QC input (file or outdir)" -a "(__fish_complete_path)"
complete -c qeeg_nf_cli -l allow-bad-metric-channels -d "Allow metric channels marked bad by --channel-qc"

# Audio feedback
complete -c qeeg_nf_cli -l audio-wav -r -d "Write reward-tone WAV" -a "(__fish_complete_path)"
complete -c qeeg_nf_cli -l audio-rate -r -d "Audio sample rate (Hz)"
complete -c qeeg_nf_cli -l audio-tone -r -d "Reward tone frequency (Hz)"
complete -c qeeg_nf_cli -l audio-gain -r -d "Reward tone gain in [0,1]"
complete -c qeeg_nf_cli -l audio-attack -r -d "Tone attack seconds"
complete -c qeeg_nf_cli -l audio-release -r -d "Tone release seconds"

# OSC output
complete -c qeeg_nf_cli -l osc-host -r -d "OSC destination host"
complete -c qeeg_nf_cli -l osc-port -r -d "OSC destination port (0 disables)"
complete -c qeeg_nf_cli -l osc-prefix -r -d "OSC address prefix"
complete -c qeeg_nf_cli -l osc-mode -r -d "OSC output mode" -a "state split bundle"

# PAC options
complete -c qeeg_nf_cli -l pac-bins -r -d "PAC: number of phase bins"
complete -c qeeg_nf_cli -l pac-trim -r -d "PAC: trim fraction per window"
complete -c qeeg_nf_cli -l pac-zero-phase -d "PAC: use zero-phase bandpass filters"

# Demo mode
complete -c qeeg_nf_cli -l demo -d "Generate synthetic recording instead of reading a file"
complete -c qeeg_nf_cli -l seconds -r -d "Duration for --demo (seconds)"
