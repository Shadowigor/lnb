{
    system("chmod " $2 " \"" dest_path "/"  $1 "\"");
    system("sudo chown " $3 " \"" dest_path "/"  $1 "\"");
}
