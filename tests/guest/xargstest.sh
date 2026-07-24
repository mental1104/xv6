/bin/mkdir a
/bin/echo hello > a/b
/bin/mkdir c
/bin/echo hello > c/b
/bin/echo hello > b
/bin/find . b | /bin/xargs /bin/grep hello
