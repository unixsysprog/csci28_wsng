/*
 * wsng.c - a web server
 *
 *    usage: ws [ -c configfilenmame ]
 * features: supports the GET command only
 *           runs in the current directory
 *           forks a new child to handle each request
 *           needs many additional features
 *
 *  compile: cc ws.c socklib.c -o ws
 *  history: 2018-04-21 added SIGINT handling (mk had it)
 *  history: 2012-04-23 removed extern declaration for fdopen (it's in stdio.h)
 *  history: 2012-04-21 more minor cleanups, expanded some fcn comments
 *  history: 2010-04-24 cleaned code, merged some of MK's ideas
 *  history: 2008-05-01 removed extra fclose that was causing double free
 */

#include    <stdio.h>
#include    <stdlib.h>
#include    <strings.h>
#include    <string.h>
#include    <netdb.h>
#include    <errno.h>
#include    <unistd.h>
#include    <sys/types.h>
#include    <sys/stat.h>
#include    <sys/param.h>
#include        <sys/wait.h>
#include    <signal.h>
#include    "socklib.h"
#include    "varlib.h"
#include    <time.h>
#include    <dirent.h>

#define PORTNUM 80
#define SERVER_ROOT "."
#define CONFIG_FILE "wsng.conf"
#define VERSION     "1"
#define SERVER_NAME "WSNG"
#define CONTENT_DEFAULT "text/plain"

#define MAX_RQ_LEN  4096
#define LINELEN     1024
#define PARAM_LEN   128
#define VALUE_LEN   512
#define CONTENT_LEN 64

char    myhost[MAXHOSTNAMELEN];
int     myport;
char    *full_hostname();

#define oops(m,x)   { perror(m); exit(x); }

/*
 * prototypes
 */
int     startup(int, char *a[], char [], int *);
void    read_til_crnl(FILE *);
void    process_rq( char *, FILE *);
void    bad_request(FILE *);
void    cannot_do(FILE *fp);
void    do_404(char *item, FILE *fp);
void    do_403(char *item, FILE *fp);
void    do_cat(char *f, FILE *fpsock);
void    do_exec( char *prog, FILE *fp);
void    do_ls(char *dir, FILE *fp);
void    do_dir(char *dir, FILE *fp);
void    output_listing(FILE * pp, FILE * fp, char *dir);
char    *get_content_type(char *ext);
int     ends_in_cgi(char *f);
char    *file_type(char *f);
void    header( FILE *fp, int code, char *msg, char *content_type );
int     isadir(char *f);
char    *modify_argument(char *arg, int len);
int     not_exist(char *f);
int     no_access(char *f);
void    fatal(char *, char *);
void    handle_call(int);
int     read_request(FILE *, char *, int);
char    *readline(char *, int, FILE *);
void    sigchld_handler(int s);
char    *parse_query(char *line);
void    process_config_type(char [PARAM_LEN],
                            char [VALUE_LEN],
                            char [CONTENT_LEN],
                            int *);
void    table_header(FILE *fp);
void    table_close(FILE *fp);
void    print_rows(FILE *fp, char *dir);
void    table_row(FILE *fp, struct dirent * dp, struct stat *info);

//from web-time.c
char * rfc822_time(time_t thetime);
char * table_time(time_t thetime);

int mysocket = -1;      /* for SIGINT handler */

int
main(int ac, char *av[])
{
    int     sock, fd;

    /* set up */
    sock = startup(ac, av, myhost, &myport);
    mysocket = sock;

    /* sign on */
    printf("wsng%s started.  host=%s port=%d\n", VERSION, myhost, myport);

    /* main loop here */
    while(1)
    {
        fd    = accept( sock, NULL, NULL ); /* take a call  */
        if ( fd == -1 )
        {
            if( errno == EINTR)             /* check if intr from sigchld */
                continue;
                
            perror("accept");
        }
        else
            handle_call(fd);                /* handle call  */
    }
    return 0;
    /* never end */
}

/*
 *  sigchld_handler()
 *  Purpose: Handle exit statuses from child process to prevent zombies
 *  Note: taken from the 'zombierace' page provided with the assignment
 *        materials for the assignment.
 */
void
sigchld_handler(int s)
{
    int old_errno = errno;
    
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
    errno = old_errno;
}

/*
 * handle_call(fd) - serve the request arriving on fd
 * summary: fork, then get request, then process request
 *    rets: child exits with 1 for error, 0 for ok
 *    note: closes fd in parent
 */
void handle_call(int fd)
{
    int     pid = fork();
    FILE    *fpin, *fpout;
    char    request[MAX_RQ_LEN];

    if ( pid == -1 ){
        perror("fork");
        return;
    }

    /* child: buffer socket and talk with client */
    if ( pid == 0 )
    {
        fpin  = fdopen(fd, "r");
        fpout = fdopen(fd, "w");
        if ( fpin == NULL || fpout == NULL )
            exit(1);

        if ( read_request(fpin, request, MAX_RQ_LEN) == -1 )
            exit(1);
        printf("got a call: request = %s", request);

        process_rq(request, fpout);
        fflush(fpout);      /* send data to client  */
        exit(0);            /* child is done    */
                            /* exit closes files    */
    }
    /* parent: close fd and return to take next call    */
    close(fd);
}

/*
 * read the http request into rq not to exceed rqlen
 * return -1 for error, 0 for success
 */
int read_request(FILE *fp, char rq[], int rqlen)
{
    /* null means EOF or error. Either way there is no request */
    if ( readline(rq, rqlen, fp) == NULL )
        return -1;
    read_til_crnl(fp);
    return 0;
}

void read_til_crnl(FILE *fp)
{
        char    buf[MAX_RQ_LEN];
        while( readline(buf,MAX_RQ_LEN,fp) != NULL 
            && strcmp(buf,"\r\n") != 0 )
                ;
}

/*
 * readline -- read in a line from fp, stop at \n 
 *    args: buf - place to store line
 *          len - size of buffer
 *          fp  - input stream
 *    rets: NULL at EOF else the buffer
 *    note: will not overflow buffer, but will read until \n or EOF
 *          thus will lose data if line exceeds len-2 chars
 *    note: like fgets but will always read until \n even if it loses data
 */
char *readline(char *buf, int len, FILE *fp)
{
        int     space = len - 2;
        char    *cp = buf;
        int     c;

        while( ( c = getc(fp) ) != '\n' && c != EOF ){
            if ( space-- > 0 )
                *cp++ = c;
        }
        if ( c == '\n' )
            *cp++ = c;
        *cp = '\0';
        return ( c == EOF && cp == buf ? NULL : buf );
}
/*
 * initialization function
 *  1. process command line args
 *      handles -c configfile
 *  2. open config file
 *      read rootdir, port
 *  3. chdir to rootdir
 *  4. open a socket on port
 *  5. gets the hostname
 *  6. return the socket
 *       later, it might set up logfiles, check config files,
 *         arrange to handle signals
 *
 *  returns: socket as the return value
 *       the host by writing it into host[]
 *       the port by writing it into *portnump
 */
int startup(int ac, char *av[], char host[], int *portnump)
{
    int sock;
    int portnum = PORTNUM;
    char *configfile = CONFIG_FILE ;
    int pos;
    void process_config_file(char *, int *);
    void done(int);

    signal(SIGINT, done);
    for(pos=1;pos<ac;pos++){
        if ( strcmp(av[pos],"-c") == 0 ){
            if ( ++pos < ac )
                configfile = av[pos];
            else
                fatal("missing arg for -c",NULL);
        }
    }
    process_config_file(configfile, &portnum);
            
    sock = make_server_socket( portnum );
    if ( sock == -1 ) 
        oops("making socket",2);
    strcpy(myhost, full_hostname());
    *portnump = portnum;
    
    signal(SIGCHLD, sigchld_handler);   /* handler for zombies */
    
    return sock;
}


/*
 * opens file or dies
 * reads file for lines with the format
 *   port ###
 *   server_root path
 * at the end, return the portnum by loading *portnump
 * and chdir to the rootdir
 */
void process_config_file(char *conf_file, int *portnump)
{
    FILE *fp;
    char rootdir[VALUE_LEN] = SERVER_ROOT;
    char param[PARAM_LEN];
    char value[VALUE_LEN];
    char type[CONTENT_LEN];
    int port;
    int read_param(FILE *, char *, int, char *, int, char *, int, int* );
    int params_read;

    /* open the file */
    if ( (fp = fopen(conf_file,"r")) == NULL )
        fatal("Cannot open config file %s", conf_file);

    /* extract the settings */
    while( read_param(fp, param, PARAM_LEN,
                          value, VALUE_LEN,
                          type, CONTENT_LEN,
                          &params_read) != EOF )
    {
        if ( strcasecmp(param,"server_root") == 0 )
            strcpy(rootdir, value);
        if ( strcasecmp(param,"port") == 0 )
            port = atoi(value);
        if ( strcasecmp(param,"type") == 0)
            process_config_type(param, value, type, &params_read);
    }
    fclose(fp);

    /* act on the settings */
    if (chdir(rootdir) == -1)
        oops("cannot change to rootdir", 2);
    *portnump = port;
    return;
}

/*
 *  process_config_type()
 *  Purpose: Store a name=value pair of extension=Content-Type
 *   Errors: If the num is not equal to three, the config file was
 *           setup wrong, or there was an error with read_param.
 */
void process_config_type(char param[PARAM_LEN],
                         char val[VALUE_LEN],
                         char type[CONTENT_LEN],
                         int *num)
{
    if (*num != 3)
    {
        fprintf(stderr, "No type specified for \"%s\"\n", val);
        return;
    }

    VLstore(val, type);
}

/*
 * read_param:
 *   purpose -- read next parameter setting line from fp
 *   details -- a param-setting line looks like  name value
 *      for example:  port 4444
 *     extra -- skip over lines that start with # and those
 *      that do not contain two strings
 *   returns -- EOF at eof and 1 on good data
 *
 */
int
read_param (FILE *fp, 
            char *name, int nlen,   // place to store name
            char* value, int vlen,  // place to store value
            char* type, int clen,   // place to store content-type
            int *num)
{
    char line[LINELEN];
    int c;
    char fmt[100] ;

    sprintf(fmt, "%%%ds%%%ds%%%ds", nlen, vlen, clen);

    /* read in next line and if the line is too long, read until \n */
    while( fgets(line, LINELEN, fp) != NULL )
    {
        if ( line[strlen(line)-1] != '\n' )
            while( (c = getc(fp)) != '\n' && c != EOF )
                ;

        // store the number of arguments read to access later
        *num = sscanf(line, fmt, name, value, type );

        // good data if not a comment (#) and either 2 or 3 args
        if ( (*num == 2 || *num == 3) && *name != '#')
            return 1;

    }
    return EOF;
}
    


/* ------------------------------------------------------ *
   process_rq( char *rq, FILE *fpout)
   do what the request asks for and write reply to fp
   rq is HTTP command:  GET /foo/bar.html HTTP/1.0
   ------------------------------------------------------ */

void process_rq(char *rq, FILE *fp)
{
    char    cmd[MAX_RQ_LEN], arg[MAX_RQ_LEN];
    char    *item, *modify_argument();

    if ( sscanf(rq, "%s%s", cmd, arg) != 2 ){
        bad_request(fp);
        return;
    }

    item = modify_argument(arg, MAX_RQ_LEN);
    item = parse_query(item);
    
    // set the request type
    if ( strcmp(cmd, "GET") == 0)
        setenv("REQUEST_METHOD", "GET", 1);
    else if ( strcmp(cmd, "HEAD") == 0 )
        setenv("REQUEST_METHOD", "HEAD", 1);
    else
    {
        cannot_do(fp);      // only supports GET or HEAD
        return;
    }

    if ( not_exist( item ) )
        do_404(item, fp );
    else if ( no_access( item) )
        do_403(item, fp);
    else if ( isadir( item ) )
        do_dir( item, fp );
    else if ( ends_in_cgi( item ) )
        do_exec( item, fp );
    else
        do_cat( item, fp );
}

/*
 *  parse_query()
 *  Purpose: Parse the line and if a query, set QUERY_STRING
 *    Input: line, the argument to parse
 *   Return: If there is a query, store it in the QUERY_STRING env
 *           variable. Return the rest of the argument, minus the query.
 */
char *
parse_query(char *line)
{
    char *query = strrchr(line, '?');

    if (query != NULL)
    {
        // set environment variable
        setenv("QUERY_STRING", (query + 1), 1);
        
        // terminate at the '?'
        *query = '\0';
    }
    
    return line;
}


/*
 * modify_argument
 *  purpose: many roles
 *      security - remove all ".." components in paths
 *      cleaning - if arg is "/" convert to "."
 *  returns: pointer to modified string
 *     args: array containing arg and length of that array
 */
char *
modify_argument(char *arg, int len)
{
    char    *nexttoken;
    char    *copy = malloc(len);

    if ( copy == NULL )
        oops("memory error", 1);

    /* remove all ".." components from path */
    /* by tokeninzing on "/" and rebuilding */
    /* the string without the ".." items    */

    *copy = '\0';

    nexttoken = strtok(arg, "/");
    while( nexttoken != NULL )
    {
        if ( strcmp(nexttoken,"..") != 0 )
        {
            if ( *copy )
                strcat(copy, "/");
            strcat(copy, nexttoken);
        }
        nexttoken = strtok(NULL, "/");
    }
    strcpy(arg, copy);
    free(copy);

    /* the array is now cleaned up */
    /* handle a special case       */

    if ( strcmp(arg,"") == 0 )
        strcpy(arg, ".");
    return arg;
}
/* ------------------------------------------------------ *
   the reply header thing: all functions need one
   if content_type is NULL then don't send content type
   ------------------------------------------------------ */

void
header( FILE *fp, int code, char *msg, char *content_type )
{
    fprintf(fp, "HTTP/1.0 %d %s\r\n", code, msg);
    fprintf(fp, "Date: %s\r\n", rfc822_time(time(0L)));
    fprintf(fp, "Server: %s/%s\r\n", SERVER_NAME, VERSION);
    
    // do not include if NULL
    if (content_type == NULL)
        return;
    // the content_type wasn't found, return the DEFAULT
    else if ( strcmp(content_type, "") == 0 )
        fprintf(fp, "Content-Type: %s\r\n", CONTENT_DEFAULT);
    // print as-is
    else
        fprintf(fp, "Content-Type: %s\r\n", content_type );
}

/* ------------------------------------------------------ *
   simple functions first:
   bad_request(fp)     bad request syntax
     cannot_do(fp)     unimplemented HTTP command
   do_404(item,fp)     no such object
   do_403(item,fp)     wrong permissions (added by MT)
   ------------------------------------------------------ */

void
bad_request(FILE *fp)
{
    header(fp, 400, "Bad Request", "text/plain");
    fprintf(fp, "\r\nI cannot understand your request\r\n");
}

void
cannot_do(FILE *fp)
{
    header(fp, 501, "Not Implemented", "text/plain");
    fprintf(fp, "\r\n");

    fprintf(fp, "That command is not yet implemented\r\n");
}

void
do_404(char *item, FILE *fp)
{
    header(fp, 404, "Not Found", "text/plain");
    fprintf(fp, "\r\n");

    fprintf(fp, "The item you requested: %s\r\nis not found\r\n", 
            item);
}

void
do_403(char *item, FILE *fp)
{
    header(fp, 403, "Forbidden", "text/plain");
    fprintf(fp, "\r\n");

    fprintf(fp, "You do not have permission to access %s on this server\r\n",
                item);
}

/* ------------------------------------------------------ *
   the directory listing section
   isadir() uses stat, not_exist() uses stat
   no_access() checks permissions of dir using stat
   do_dir() checks if an 'index.html' or 'index.cgi'
        file exists. If yes, it outputs that, otherwise
        calls do_ls().
   do_ls() opens a pipe to a listing of the directory.
        For each entry, it formats the line with a link
        to that file.
   ------------------------------------------------------ */

int
isadir(char *f)
{
    struct stat info;
    return ( stat(f, &info) != -1 && S_ISDIR(info.st_mode) );
}

int
not_exist(char *f)
{
    struct stat info;

    return( stat(f,&info) == -1 && errno == ENOENT );
}

/*
 *  no_access()
 *  Purpose: check permissions of page/file trying to be loaded
 *    Input: f, the name of the file
 *   Return: 1, if not allowed to access; 0 otherwise
 *     Note: added by MT, used to detect 403 error
 */
int
no_access(char *f)
{
    struct stat info;
    char path[LINELEN];

    // construct the path to the file
    strcpy(path, "./");
    snprintf(path, LINELEN, "%s/", f);

    // get the permissions info
    if( stat(path, &info) != -1 )
    {
        // there is no access allowed, return 1 to direct to a 403
        if(! (S_IRUSR & info.st_mode) || ! (S_IXUSR & info.st_mode) )
            return 1;
    }
    return 0;

}

/*
 *  do_dir()
 *  Purpose: check the current directory to see if an index file exists
 */
void
do_dir(char *dir, FILE *fp)
{
    struct stat info;
    char html[LINELEN];
    char cgi[LINELEN];
    
    // create a path to check HTML index
    strcpy(html, dir);
    strcat(html, "/index.html");
    
    // create a path to check CGI index
    strcpy(cgi, dir);
    strcat(cgi, "/index.cgi");

    if(stat(html, &info) == 0 )     // html exists
        do_cat(html, fp);
    else if (stat(cgi, &info) == 0) // cgi exists
        do_exec(cgi, fp);
    else                            // no index, output listing
        do_ls(dir, fp);
    
    return;
}

/*
 * lists the directory named by 'dir' 
 * sends the listing to the stream at fp
 *
 * Note: Modified for the assignment. 
 */
void
do_ls(char *dir, FILE *fp)
{
    header(fp, 200, "OK", "text/html");
    fprintf(fp,"\r\n");
    
    table_header(fp);
    
    print_rows(fp, dir);
    
    table_close(fp);
}

/*
 *  construct_path()
 *  Purpose: concatenate a parent and child into a full path name
 *    Input: parent, current path to the open directory
 *           child, name of the last entry read by readdir()
 *   Return: pointer to full path string allocated by malloc()
 *   Errors: if malloc() or sprintf() fail, return NULL. This will
 *           cause lstat() to output an error back in process_file or
 *           process_dir().
 *   Method: Start by malloc()ing enough memory to store the combined
 *           path. If sucessful, call sprintf() to copy into "newstr":
 *           1) just the parent, if parent and child are the same;
 *           2) if parent or child has trailing or leading '/',
 *              respectively, do not copy an extra '/'; or
 *           3) concatenate "parent/child"
 *     Note: This function was copied from my pfind.c assignment as-is.
 */
char * construct_path(char *parent, char *child)
{
    int rv;
    int path_size = 1 + strlen(parent) + 1 + strlen(child);
    char *newstr = malloc(path_size);

    //Check malloc() returned memory. If no, lstat() will output error
    if (newstr == NULL)
        return NULL;

    //Concatenate "parent/child", see Method above for how
    if (strcmp(parent, child) == 0)
        rv = sprintf(newstr, "%s", parent);
    else if (parent[strlen(parent) - 1] == '/' || child[0] == '/')
        rv = sprintf(newstr, "%s%s", parent, child);
    else
        rv = sprintf(newstr, "%s/%s", parent, child);

    //check for sprintf error --or-- overflow error
    if ( rv < 0 || rv > (path_size - 1) )
    {
        free(newstr);       //failed to construct path
        return NULL;        //return will cause lstat() error
    }

    return newstr;
}

/*
 *  print_rows() -- open the directory specified by "dir", and call on
 *      table_row() for each file it finds.
 *  Note: This code was copied from my pfind assignment. The while loop
 *      processing was altered to fit the webserver requirements.
 */
void
print_rows(FILE *fp, char *dir)
{
    DIR* list = opendir(dir);
    
    if(list == NULL)
    {
        fprintf(stderr, "Couldn't open directory\n");
        return;
    }
    
    struct dirent *dp = NULL;
    struct stat info;
    char *path = NULL;
    
    // output a row for each file
    while( (dp = readdir(list)) != NULL)
    {
        path = construct_path(dir, dp->d_name);
        
        if (lstat(path, &info) == -1)
        {
            fprintf(stderr, "error with %s\n", path);
            continue;
        }
        
        // format the row data
        table_row(fp, dp, &info);
        
        // prevent memory leaks
        if(path != NULL)
            free(path);
    }
}

/*
 *  table_row() -- output an HTML formatted table row containing:
 *          Name (with link to file), Last Modified time, and file size
 */
void
table_row(FILE *fp, struct dirent * dp, struct stat *info)
{
    fprintf(fp, "<tr><td>");
    
    // add a trailing '/' if the file is a directory
    if(S_ISDIR(info->st_mode))
        fprintf(fp, "<a href='%s/'>%s</a>", dp->d_name, dp->d_name);
    else
        fprintf(fp, "<a href='%s'>%s</a>", dp->d_name, dp->d_name);
    
    fprintf(fp, "</td>");
    
    // output Last Modified time
    fprintf(fp, "<td>");
    fprintf(fp, "%s", table_time(info->st_mtime));
    fprintf(fp, "</td>");
    
    // out file size
    fprintf(fp, "<td>");
    fprintf(fp, "%d", (int) info->st_size);
    fprintf(fp, "</td></tr>");
    
}

/*
 *  table_header() -- Output opening tags for an HTML table and header row
 *      containing: Name, Last Modified, and Size
 */
void
table_header(FILE *fp)
{
    fprintf(fp, "<table>\n<tbody>\n<tr>");
    fprintf(fp, "<th>Name</th>");
    fprintf(fp, "<th>Last Modified</th>");
    fprintf(fp, "<th>Size</th>");
    fprintf(fp, "</tr>\n");
}

/*
 *  table_close() -- output closing HTML table tags
 */
void
table_close(FILE *fp)
{
    fprintf(fp, "</tbody></table>\n");
}

/* ------------------------------------------------------ *
   the cgi stuff.  function to check extension and
   one to run the program.
   ------------------------------------------------------ */

char *
file_type(char *f)
/* returns 'extension' of file */
{
    char    *cp;
    if ( (cp = strrchr(f, '.' )) != NULL )
        return cp+1;
    return "";
}

int
ends_in_cgi(char *f)
{
    return ( strcmp( file_type(f), "cgi" ) == 0 );
}

void
do_exec( char *prog, FILE *fp)
{
    int fd = fileno(fp);

    header(fp, 200, "OK", NULL);
    fflush(fp);

    dup2(fd, 1);
    dup2(fd, 2);
    execl(prog,prog,NULL);
    perror(prog);
}
/* ------------------------------------------------------ *
   do_cat(filename,fp)
   sends back contents after a header
   ------------------------------------------------------ */

/*
 *  Modified from starter code. Moved Content-Type from if/else
 *  switch, to a table-driven design. See varlib.c for more.
 */
void
do_cat(char *f, FILE *fpsock)
{
    char    *extension = file_type(f);
    char    *content = VLlookup(extension);

    FILE    *fpfile;
    int c;

    fpfile = fopen( f , "r");
    if ( fpfile != NULL )
    {
        header( fpsock, 200, "OK", content );
        fprintf(fpsock, "\r\n");
        while( (c = getc(fpfile) ) != EOF )
            putc(c, fpsock);
        fclose(fpfile);
    }
}

char *
full_hostname()
/*
 * returns full `official' hostname for current machine
 * NOTE: this returns a ptr to a static buffer that is
 *       overwritten with each call. ( you know what to do.)
 */
{
    struct  hostent     *hp;
    char    hname[MAXHOSTNAMELEN];
    static  char fullname[MAXHOSTNAMELEN];

    if ( gethostname(hname,MAXHOSTNAMELEN) == -1 )  /* get rel name */
    {
        perror("gethostname");
        exit(1);
    }
    hp = gethostbyname( hname );        /* get info about host  */
    if ( hp == NULL )           /*   or die     */
        return NULL;
    strcpy( fullname, hp->h_name );     /* store foo.bar.com    */
    return fullname;            /* and return it    */
}


void fatal(char *fmt, char *str)
{
    fprintf(stderr, fmt, str);
    exit(1);
}

void done(int n)
{
    if ( mysocket != -1 ){
        fprintf(stderr, "closing socket\n");
        close(mysocket);
    }
    exit(0);
}
