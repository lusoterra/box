# fish completion for box (BOoru eXplorer)
# Install: cp assets/completions/box.fish /usr/share/fish/vendor_completions.d/box.fish

complete -c box -f

complete -c box -l stylesheet -r -d 'Apply a custom Qt stylesheet for this run' \
	-a '(__fish_complete_suffix .qss .css)'
complete -c box -l native-theme -d 'Use the desktop Qt theme instead of the built-in one'
complete -c box -s h -l help -d 'Show help and exit'
complete -c box -l help-all -d 'Show help including Qt options and exit'

# Positional ROOT: a folder to scan.
complete -c box -n '__fish_use_subcommand' -a '(__fish_complete_directories)'
