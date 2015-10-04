awk -F $'\t' -v dest_path="$1/$2" -f '/tmp/lnb_scripts/create_dirs.awk' "$1/.meta/$2/lnb_fileindexer_dirs"
