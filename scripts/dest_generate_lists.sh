comm -12 "$1/.meta/$3/lnb_fileindexer_files" "$1/.meta/$2/lnb_fileindexer_files" > '/tmp/lnb_to_link'
comm -23 "$1/.meta/$3/lnb_fileindexer_files" "$1/.meta/$2/lnb_fileindexer_files" | awk -F $'\t' '{print $1}' > '/tmp/lnb_to_copy'
