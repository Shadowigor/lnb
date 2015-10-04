sort '/tmp/lnb_fileindexer_files' > '/tmp/lnb_fileindexer_files_sorted'
sort -t$'\t' -k1 -V '/tmp/lnb_fileindexer_dirs' > '/tmp/lnb_fileindexer_dirs_sorted'
mv '/tmp/lnb_fileindexer_files_sorted' '/tmp/lnb_fileindexer_files'
mv '/tmp/lnb_fileindexer_dirs_sorted' '/tmp/lnb_fileindexer_dirs'
