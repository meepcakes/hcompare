# hcompare

Walk a directory and give MD5 checksums of all files

Compile with: (x86) gcc -o hcompare hcompare.c
              (armle-v7) QNX Momentics IDE

Usage: hcompare -b <Buffer> -f <File> -d <Directory> [-r] [-k] [-v <Verbosity>]
  Options:
    -b <Buffer>    : Buffer Size. Default is (5 MB)
    -f <File>      : File name to be generated or read in. (Example: /fs/mmc1/ref.txt)
    -d <Directory> : Directory to walk and generate MD5s for (Example: /fs/mmc1)
    -r             : If set, will execute creation
    -k             : If set, will run program in analysis mode
    -v <Verbosity> : Verbose Output

main git repo:  https://hzqmrk@bitbucket.org:443/hzqmrk/hcompare
                ssh://hzqmrk@bitbucket.org/hzqmrk/hcompare - does not work
