# bash completion for box (BOoru eXplorer)
# Install: cp assets/completions/box.bash /usr/share/bash-completion/completions/box

_box_completion() {
	local cur prev
	cur=${COMP_WORDS[COMP_CWORD]}
	prev=${COMP_WORDS[COMP_CWORD - 1]}

	case "$prev" in
	--stylesheet)
		# Qt stylesheets; file contents are not validated until launch.
		_filedir '@(qss|css)'
		return
		;;
	esac

	case "$cur" in
	-*)
		COMPREPLY=($(compgen -W '--stylesheet --native-theme --help --help-all' -- "$cur"))
		return
		;;
	esac

	# ROOT is a folder to scan; options may come before or after it.
	_filedir -d
}

complete -F _box_completion box
