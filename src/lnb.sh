#!/bin/bash

# Standard configuration

config_file="/home/alain/.lnb.conf"
config_valid=""
dest_host=""
dest_port=22
dest_ssh=""
dest_ssh_rsync=""
dest_path=""
src_host=""
src_port=22
src_ssh=""
src_path=""
exclude=""
last_name=""
next_name=""
manual=0
TAB=$'\t'

# Get config
# ----------

if [[ -f "$config_file" ]]; then 
    # Read file
    source "$config_file"
fi

# Parse command line arguments
while [[ $# > 0 ]]
    do
    key="$1"

    case $key in
        -dh|--dest-host)
            dest_host="$2"
            shift
            ;;

        -sh|--src-host)
            src_host="$2"
            shift
            ;;

        -sp|--src-port)
            src_port=$2
            shift
            ;;

        -dp|--dest-port)
            dest_port=$2
            shift
            ;;

        -s|--source)
            src_path="$2"
            shift
            ;;

        -d|--dest)
            dest_path="$2"
            shift
            ;;

        -l|--last)
            last_name="$2"
            shift
            ;;

        -n|--next)
            next_name="$2"
            shift
            ;;

        -e|--exclude)
            exclude="$exclude $2"
            shift
            ;;

        -m|--manual)
            manual=1
            ;;

        -h|--help)
            echo "Usage: lnb [OPTIONS]"
            exit 0
            ;;

        *)
            echo "Unknown option '$key'"
            exit 64 # EX_USAGE
            ;;
    esac
    shift
done

if [[ -z "$dest_path" ]]; then
    echo "Error: Destination path not set" > /dev/stderr
    exit 22 # EINVAL
fi

if [[ -z "$src_path" ]]; then
    echo "Error: Source path not set" > /dev/stderr
    exit 22 # EINVAL
fi

if [[ -z "$last_name" ]] && [[ $manual -ne 0 ]]; then
    echo "Error: Name of last backup not set" > /dev/stderr
    exit 22 # EINVAL
fi

if [[ -z "$next_name" ]] && [[ $manual -ne 0 ]]; then
    echo "Error: Name of backup not set" > /dev/stderr
    exit 22 # EINVAL
fi

id -u > /dev/null
if [[ $? -ne 0 ]]; then
    echo -n "Warning: Not running this program as root might lead to an incomplete backup! Continue? [Y/n] " > /dev/stderr
    read user_input
    if [[ $? -ne 0 ]] || ( [[ $user_input != "y" ]] && [[ -n $user_input ]] ); then
        exit 1 # EPERM
    fi
fi

# Execute command on source machine
function onsrc
{
    if [[ -n $src_host ]]; then
        echo "$@" | ssh -q $src_ssh $src_host
    else
	    eval "$@"
    fi
}

# Execute command on destination machine
function ondest
{
    if [[ -n $dest_host ]]; then
        echo "$@" | ssh -q $dest_ssh $dest_host
    else
        eval "$@"
    fi
}

# Initialize
# ----------

if [[ -n $src_host ]]; then
    # Set path where the ssh session will be stored
    src_ssh="-p $src_port -o ServerAliveInterval=100 -S \"$HOME/.ssh/%L-%r@%h:%p\""

    # Open ssh tunnel
    ssh -nNMf $src_ssh $src_host
    
    if [[ $? -ne 0 ]]; then
        echo "Error: Unable to connect to source via ssh"
        exit 1
    fi
    
    # Modify ssh variable for rsync
    src_ssh_rsync="-e \"ssh $src_ssh\""
    
    # Fuck all those goddamn quotes!!!
    onsrc "mkdir '/tmp/lnb_scripts'"
    rsync -e "ssh $src_ssh" /usr/share/lnb/* $src_host:'/tmp/lnb_scripts'
else
    mkdir '/tmp/lnb_scripts'
    cp /usr/share/lnb/* '/tmp/lnb_scripts'
fi

if [[ -n $dest_host ]]; then
    # Set path where the ssh session will be stored
    if [[ -n $src_host ]]; then
        dest_ssh="-p $dest_port -o ServerAliveInterval=100 -S \$HOME/.ssh/%L-%r@%h:%p"
    else
        dest_ssh="-p $dest_port -o ServerAliveInterval=100 -S $HOME/.ssh/%L-%r@%h:%p"
    fi

    # Open ssh tunnel
    onsrc "ssh -nNMf $dest_ssh $dest_host"
    
    if [[ $? -ne 0 ]]; then
        echo "Error: Unable to connect to destination via ssh"
        exit 11 # EAGAIN
    fi

    # Combine destination host with path
    dest_full=$dest_host:"$dest_path"
    
    # Modify ssh variable for rsync
    dest_ssh_rsync="-e \"ssh $dest_ssh\""

    ondest "mkdir '/tmp/lnb_scripts'"
    rsync -e "ssh $dest_ssh" /usr/share/lnb/* $dest_host:'/tmp/lnb_scripts'
else
    if [[ -n $src_host ]]; then
        mkdir '/tmp/lnb_scripts' > /dev/null
        cp /usr/share/lnb/* '/tmp/lnb_scripts'
    fi

    # Initialize full destination path
    dest_full="$dest_path"
fi

if [[ $manual -eq 0 ]]; then
    IFS=' ' read -a names <<< "$(ondest "bash '/tmp/lnb_scripts/daily_pre.sh' '$dest_path'")"
    last_name=${names[0]}
    next_name=${names[1]}
    dest_path="$dest_path/raw"
    dest_full="$dest_full/raw"
fi

if [[ -z "$(ondest "bash -c 'if [[ -e \'$dest_path/$last_name\' ]]; then echo \'e\'; fi'")" ]]; then
    echo "Error: There is no base for the backup" > /dev/stderr
    exit 2 # ENOENT
fi

# Remove previous backups with the same name
ondest "sudo rm -rf '$dest_path/$next_name' '$dest_path/.meta/$next_name'"

# Create destination root
ondest "mkdir -p '$dest_path/$next_name'"

# Create destination metadata directory for next backup
ondest "mkdir -p '$dest_path/.meta/$next_name'"

# Sync
# ----

# Generate current metadata
onsrc "lnb_fileindexer '$src_path' $exclude"

# Sort files
onsrc "bash /tmp/lnb_scripts/src_sort_files.sh"

# Write current metadata to destination
onsrc "rsync $dest_ssh_rsync /tmp/lnb_fileindexer* '$dest_full/.meta/$next_name'"

# Generate file lists
ondest "bash /tmp/lnb_scripts/dest_generate_lists.sh '$dest_path' '$last_name' '$next_name'"

if [[ -n $dest_host ]]; then
    # Get files that need to be copied
    onsrc "rsync $dest_ssh_rsync $dest_host:/tmp/lnb_to_copy /tmp/lnb_to_copy"
fi

# Backup
# ------

# Create complete directory structure
ondest "bash /tmp/lnb_scripts/dest_mkdir.sh '$dest_path' '$next_name'"

# Copy files that changed
onsrc "rsync $dest_ssh_rsync -azH --info=progress2 --files-from='/tmp/lnb_to_copy' '$src_path' '$dest_full/$next_name'"

# Link the rest of the files
ondest "awk -F '$TAB' -v last_path='$dest_path/$last_name' -v next_path='$dest_path/$next_name' -f '/tmp/lnb_scripts/link_files.awk' /tmp/lnb_to_link"

# Set permissions
ondest "bash /tmp/lnb_scripts/dest_set_perms.sh '$dest_path' '$next_name'"

# Finalize
# --------

# Link backup order
if [[ $manual -eq 0 ]]; then
    ondest "bash '/tmp/lnb_scripts/daily_post.sh' '$dest_path'"
fi

# Remove temp files
onsrc "rm -rf /tmp/lnb*"
ondest "rm -rf /tmp/lnb*"

if [[ -n $dest_host ]]; then 
    # Close ssh tunnel
    onsrc "ssh -O exit $dest_ssh $dest_host"
fi

if [[ -n $src_host ]]; then 
    # Close ssh tunnel
    ssh -q -O exit $src_ssh $src_host
fi
