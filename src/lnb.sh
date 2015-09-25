#!/bin/bash

# Standard configuration

config_file="~/.lnb.conf
remote_host=""
remote_port=22
source_path=""
dest_path=""
ssh_var=""
bu_type="MANUAL"

# Get config
# ----------

# Check if config file contains any invalid/malicious content
config_valid=`$(cat "$config_file" | grep -Evi "^(#.*|[a-z]*='[a-z0-9 ]*')$")

if [ -n "$config_valid" ]; then
    echo "Error in config file. Not allowed lines:"
    echo $config_valid
    exit 1
fi

# Read file
source "$config_file"

# Parse command line arguments
while [[ $# > 0 ]]
    do
    key="$1"

    case $key in
        -r|--remote)
            remote_host="$2"
            shift
            ;;

        -p|--port)
            remote_port="$2"
            shift
            ;;

        -s|--source)
            sourch_path="$2"
            shift
            ;;

        -d|--dest)
            dest_path="$2"
            shift
            ;;

        -b|--base)
            last_bu="$2"
            shift
            ;;

        -h|--help)
            echo "Usage: lnb [OPTIONS]"
            ;;

        *)
            echo "Unknown option 'key'"
            exit 64 # EX_USAGE
            ;;
    esac
    shift
done

# Execute command locally or remotely
function exon {
    if [[ -n $remote_host ]]; then
        echo "$@" | ssh $sshvar $remote_host
	else
	    "$@"
    fi
}

# Initialize
# ----------

dest_full="$dest_path"

if [[ -n $remote_host ]]; then
    # Set path where the ssh session will be stored
    sshvar=-S "$HOME/.ssh/ctl/%L-%r@%h:%p"

    # Open ssh tunnel
    ssh -nNM -p $remote_port $ssvar $remote_host

    # Combine remote host with path
    dest_full=$remote_host:"$dest_path"
fi

# Create destination root
exon mkdir -p "$dest_path"

# Sync
# ----

# Create dir where the metadata of the last backup will be stored
mkdir /tmp/lnb_dest

# Get metadata of last backup
rsync $sshvar "$dest_full/tmp/*" /tmp/lnb_dest

# Generate current metadata
lnb_fileindexer $source_path

# Generate file lists
comm -12 /tmp/lnb_fileindexer_files /tmp/lnb_dest/lnb_fileindexer_files > /tmp/lnb_to_link
comm -23 /tmp/lnb_fileindexer_files /tmp/lnb_dest/lnb_fileindexer_files > /tmp/lnb_to_copy

# Backup
# ------

# Create complete directory structure
exon awk "{system(\"mkdir -p \"$dest_path/\$1\"\");}" "/tmp/lnb_fileindexer_dirs"

# Copy files that changed
rsync $sshvar -az --info=progress2 --files-from="/tmp/lnb_fileindexer_to_copy" / "$dest_full"

# Link the rest of the files
exon awk "{system(\"ln \"$last_bu/\$1\" \"$dest_path\$2\"\");}" "/tmp/lnb_fileindexer_to_link"

# Finalize
# --------

# Copy current metadata to backup
exon mkdir "$dest_full/tmp"
rsync $sshvar /tmp/lnb_fileindexer_files "$dest_full/tmp/lnb_fileindexer_files"

# Remove temp files
rm -r /tmp/lnb*

if [[ -n $remote_host ]]; then 
    # Close ssh tunnel
    ssh -O exit -p $remote_port $sshvar $remote_host
fi
