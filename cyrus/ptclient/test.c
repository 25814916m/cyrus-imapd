#include <stdio.h>
#include <sys/syslog.h>
int main(int argc, char **argv) {
  char *cacheid;
  char cache[16];
  
  if (argc < 3 || argc > 4) {
    printf("Usage: pttset user group [cachearg]\n");
    exit(1);
  }
  if (argc == 4) {
    memset(cache,0,16);
    strncpy(cache,argv[3],16);
    cacheid=cache;
  } else
    cacheid=NULL;
  openlog("pttest", LOG_PID,LOG_LOCAL6);  
  
  if (!auth_setid(argv[1],cacheid))
    printf ("Auth_memberof(%s,%s) is %d\n", argv[1], argv[2],
            auth_memberof(argv[2]));
  
  else
    printf ("Auth_setid(%s) failed\n", argv[1]);
  
}

int fatal(char *foo) {
  fprintf(stderr, "Fatal error: %s\n", foo);
  exit(1);
}
