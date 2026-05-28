/*
 * file_server.c - Bai 2: HTTP file server duyet thu muc va phuc vu tap tin
 *
 *   GET  /            -> liet ke thu muc hien tai (thu muc in dam, file in nghieng)
 *   GET  /path/dir/   -> liet ke thu muc con
 *   GET  /path/file   -> tra ve noi dung file (text, anh, audio, video)
 *
 * Bien dich Linux : gcc -o file_server file_server.c
 * Bien dich Windows: gcc -o file_server file_server.c -lws2_32
 *
 * Chay: ./file_server <port> [so_worker]
 *       Server phuc vu file tu thu muc hien tai luc chay lenh.
 */

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET      socket_t;
typedef int         io_result_t;
#define close_socket closesocket
#else
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
typedef int         socket_t;
typedef ssize_t     io_result_t;
#define INVALID_SOCKET  (-1)
#define SOCKET_ERROR    (-1)
#define close_socket    close
#endif

#define BUFFER_SIZE   8192
#define MAX_PATH_LEN  1024

/* ---- khoi tao socket ---- */

static int initialize_socket_system(void) {
#ifdef _WIN32
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0 ? 0 : -1;
#else
    return 0;
#endif
}

static void cleanup_socket_system(void) {
#ifdef _WIN32
    WSACleanup();
#endif
}

#ifndef _WIN32
static void reap_children(int sig) {
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0) {}
}
#endif

/* ---- gui du lieu ---- */

static int send_all(socket_t fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        io_result_t n = send(fd, buf + sent, (int)(len - sent), 0);
        if (n < 0) {
#ifndef _WIN32
            if (errno == EINTR) continue;
#endif
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

/* ---- bo dem chuoi dong (de xay dung HTML) ---- */

typedef struct { char *data; size_t len; size_t cap; } strbuf_t;

static strbuf_t strbuf_new(void) {
    strbuf_t sb; sb.data=NULL; sb.len=0; sb.cap=0; return sb;
}

static int strbuf_ensure(strbuf_t *sb, size_t needed) {
    size_t nc; char *nd;
    if (sb->len + needed <= sb->cap) return 0;
    nc = (sb->cap==0) ? 4096 : sb->cap*2;
    while (nc < sb->len+needed) nc*=2;
    nd = (char*)realloc(sb->data, nc);
    if (!nd) return -1;
    sb->data=nd; sb->cap=nc; return 0;
}

static int strbuf_append(strbuf_t *sb, const char *str) {
    size_t slen=strlen(str);
    if (strbuf_ensure(sb,slen+1)<0) return -1;
    memcpy(sb->data+sb->len, str, slen+1);
    sb->len+=slen; return 0;
}

static int strbuf_appendf(strbuf_t *sb, const char *fmt, ...) {
    char tmp[2048]; va_list ap;
    va_start(ap,fmt); vsnprintf(tmp,sizeof(tmp),fmt,ap); va_end(ap);
    return strbuf_append(sb,tmp);
}

static void strbuf_free(strbuf_t *sb) {
    free(sb->data); sb->data=NULL; sb->len=0; sb->cap=0;
}

/* ---- ma hoa / giai ma ---- */

static int hex_val(char c) {
    if (c>='0'&&c<='9') return c-'0';
    if (c>='a'&&c<='f') return c-'a'+10;
    if (c>='A'&&c<='F') return c-'A'+10;
    return -1;
}

static void url_decode(const char *src, char *dst, size_t dst_size) {
    size_t i=0;
    while (*src && i+1<dst_size) {
        if (*src=='%'&&src[1]&&src[2]) {
            int hi=hex_val(src[1]), lo=hex_val(src[2]);
            if (hi>=0&&lo>=0) { dst[i++]=(char)(hi*16+lo); src+=3; continue; }
        }
        dst[i++]=*src++;
    }
    dst[i]='\0';
}

static void url_encode_name(const char *src, char *dst, size_t dst_size) {
    static const char hex[]="0123456789ABCDEF";
    size_t i=0;
    while (*src && i+3<dst_size) {
        unsigned char c=(unsigned char)*src;
        if (isalnum(c)||c=='-'||c=='_'||c=='.'||c=='~') { dst[i++]=(char)c; }
        else { dst[i++]='%'; dst[i++]=hex[c>>4]; dst[i++]=hex[c&0xF]; }
        src++;
    }
    dst[i]='\0';
}

static void html_encode(const char *src, char *dst, size_t dst_size) {
    size_t i=0;
    while (*src && i+6<dst_size) {
        if      (*src=='<') { memcpy(dst+i,"&lt;",  4); i+=4; }
        else if (*src=='>') { memcpy(dst+i,"&gt;",  4); i+=4; }
        else if (*src=='&') { memcpy(dst+i,"&amp;", 5); i+=5; }
        else if (*src=='"') { memcpy(dst+i,"&quot;",6); i+=6; }
        else                { dst[i++]=*src; }
        src++;
    }
    dst[i]='\0';
}

/* ---- url path -> filesystem path (ngan path traversal) ---- */

static int url_to_fs_path(const char *url_path, char *fs_path, size_t fs_size) {
    char decoded[MAX_PATH_LEN];
    const char *q = strchr(url_path,'?');
    char tmp[MAX_PATH_LEN];
    const char *p;
    size_t i;
    if (q) {
        size_t plen=(size_t)(q-url_path);
        if (plen>=sizeof(tmp)) return -1;
        memcpy(tmp,url_path,plen); tmp[plen]='\0';
        url_decode(tmp,decoded,sizeof(decoded));
    } else {
        url_decode(url_path,decoded,sizeof(decoded));
    }
    if (decoded[0]!='/' && decoded[0]!='\0') return -1;
    if (strstr(decoded,"..") != NULL) return -1;
    if (fs_size < 2) return -1;
    fs_path[0]='.'; i=1; p=decoded;
    while (*p && i+1<fs_size) { fs_path[i++]=*p++; }
    if (*p) return -1;
    while (i>1 && fs_path[i-1]=='/') i--;
    fs_path[i]='\0';
    return 0;
}

/* ---- content-type ---- */

static const char *content_type_for(const char *path) {
    const char *ext=strrchr(path,'.');
    if (!ext) return "application/octet-stream";
    if (strcmp(ext,".html")==0||strcmp(ext,".htm")==0) return "text/html; charset=utf-8";
    if (strcmp(ext,".txt")==0)  return "text/plain; charset=utf-8";
    if (strcmp(ext,".css")==0)  return "text/css";
    if (strcmp(ext,".js")==0)   return "application/javascript";
    if (strcmp(ext,".json")==0) return "application/json";
    if (strcmp(ext,".c")==0||strcmp(ext,".h")==0||strcmp(ext,".py")==0||strcmp(ext,".md")==0)
        return "text/plain; charset=utf-8";
    if (strcmp(ext,".jpg")==0||strcmp(ext,".jpeg")==0) return "image/jpeg";
    if (strcmp(ext,".png")==0)  return "image/png";
    if (strcmp(ext,".gif")==0)  return "image/gif";
    if (strcmp(ext,".webp")==0) return "image/webp";
    if (strcmp(ext,".svg")==0)  return "image/svg+xml";
    if (strcmp(ext,".bmp")==0)  return "image/bmp";
    if (strcmp(ext,".ico")==0)  return "image/x-icon";
    if (strcmp(ext,".mp3")==0)  return "audio/mpeg";
    if (strcmp(ext,".wav")==0)  return "audio/wav";
    if (strcmp(ext,".ogg")==0)  return "audio/ogg";
    if (strcmp(ext,".flac")==0) return "audio/flac";
    if (strcmp(ext,".aac")==0)  return "audio/aac";
    if (strcmp(ext,".m4a")==0)  return "audio/mp4";
    if (strcmp(ext,".mp4")==0)  return "video/mp4";
    if (strcmp(ext,".webm")==0) return "video/webm";
    if (strcmp(ext,".avi")==0)  return "video/x-msvideo";
    if (strcmp(ext,".mkv")==0)  return "video/x-matroska";
    if (strcmp(ext,".mov")==0)  return "video/quicktime";
    if (strcmp(ext,".wmv")==0)  return "video/x-ms-wmv";
    return "application/octet-stream";
}

/* ---- kiem tra loai duong dan ---- */

static int path_is_directory(const char *path) {
#ifdef _WIN32
    DWORD attr=GetFileAttributesA(path);
    return attr!=INVALID_FILE_ATTRIBUTES && (attr&FILE_ATTRIBUTE_DIRECTORY)!=0;
#else
    struct stat st;
    return stat(path,&st)==0 && S_ISDIR(st.st_mode);
#endif
}

static int path_exists(const char *path) {
#ifdef _WIN32
    return GetFileAttributesA(path)!=INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    return stat(path,&st)==0;
#endif
}

/* ---- tao trang HTML liet ke thu muc ---- */
/* Thu muc: <b>in dam</b>   File: <i>in nghieng</i>  */

static char *generate_dir_listing(const char *fs_path, const char *url_path) {
    strbuf_t sb=strbuf_new();
    char safe_url[MAX_PATH_LEN];
    char safe_name[1024], enc_name[1024], href[MAX_PATH_LEN+1024];
    size_t url_len;
    const char *q=strchr(url_path,'?');
    size_t len = q ? (size_t)(q-url_path) : strlen(url_path);
    if (len>=sizeof(safe_url)) len=sizeof(safe_url)-1;
    memcpy(safe_url,url_path,len); safe_url[len]='\0';
    url_len=strlen(safe_url);
    if (url_len==0||safe_url[url_len-1]!='/') {
        if (url_len+2<sizeof(safe_url)) { safe_url[url_len]='/'; safe_url[url_len+1]='\0'; }
    }
    strbuf_appendf(&sb,
        "<!DOCTYPE html>\n<html>\n"
        "<head><meta charset='utf-8'><title>Thu muc: %s</title>\n"
        "<style>\n"
        "  body{font-family:Arial,sans-serif;max-width:800px;margin:30px auto}\n"
        "  h2{color:#333;border-bottom:2px solid #0078d4;padding-bottom:6px}\n"
        "  ul{list-style:none;padding:0}\n"
        "  li{padding:5px 8px;border-bottom:1px solid #eee}\n"
        "  li:hover{background:#f5f5f5}\n"
        "  a{text-decoration:none;color:#0078d4}\n"
        "  a:hover{text-decoration:underline}\n"
        "  b{color:#1a5276}\n"
        "  i{color:#555}\n"
        "  .size{color:#999;font-size:0.85em;margin-left:8px}\n"
        "</style>\n</head>\n<body>\n"
        "<h2>&#128193; Thu muc: %s</h2>\n<ul>\n",
        safe_url, safe_url);
    if (strcmp(safe_url,"/")!=0)
        strbuf_append(&sb,"<li><a href='../'><b>..</b> <span class='size'>(Thu muc cha)</span></a></li>\n");
#ifdef _WIN32
    {
        WIN32_FIND_DATAA fd_data; HANDLE h; char pat[MAX_PATH_LEN];
        snprintf(pat,sizeof(pat),"%s/*",fs_path);
        /* Thu muc */
        h=FindFirstFileA(pat,&fd_data);
        if (h!=INVALID_HANDLE_VALUE) {
            do {
                if (strcmp(fd_data.cFileName,".")==0||strcmp(fd_data.cFileName,"..")==0) continue;
                if (!(fd_data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)) continue;
                html_encode(fd_data.cFileName,safe_name,sizeof(safe_name));
                url_encode_name(fd_data.cFileName,enc_name,sizeof(enc_name));
                snprintf(href,sizeof(href),"%s%s/",safe_url,enc_name);
                strbuf_appendf(&sb,"<li><a href='%s'>&#128193; <b>%s/</b></a></li>\n",href,safe_name);
            } while (FindNextFileA(h,&fd_data));
            FindClose(h);
        }
        /* File */
        h=FindFirstFileA(pat,&fd_data);
        if (h!=INVALID_HANDLE_VALUE) {
            do {
                long long sz;
                if (strcmp(fd_data.cFileName,".")==0||strcmp(fd_data.cFileName,"..")==0) continue;
                if (fd_data.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY) continue;
                sz=((long long)fd_data.nFileSizeHigh<<32)|(long long)fd_data.nFileSizeLow;
                html_encode(fd_data.cFileName,safe_name,sizeof(safe_name));
                url_encode_name(fd_data.cFileName,enc_name,sizeof(enc_name));
                snprintf(href,sizeof(href),"%s%s",safe_url,enc_name);
                strbuf_appendf(&sb,"<li><a href='%s'>&#128196; <i>%s</i></a><span class='size'>%lld bytes</span></li>\n",href,safe_name,sz);
            } while (FindNextFileA(h,&fd_data));
            FindClose(h);
        }
    }
#else
    {
        DIR *dir_p=opendir(fs_path);
        if (dir_p) {
            struct dirent *entry;
            /* Thu muc */
            while ((entry=readdir(dir_p))!=NULL) {
                struct stat st; char full[MAX_PATH_LEN];
                if (strcmp(entry->d_name,".")==0||strcmp(entry->d_name,"..")==0) continue;
                snprintf(full,sizeof(full),"%s/%s",fs_path,entry->d_name);
                if (stat(full,&st)!=0||!S_ISDIR(st.st_mode)) continue;
                html_encode(entry->d_name,safe_name,sizeof(safe_name));
                url_encode_name(entry->d_name,enc_name,sizeof(enc_name));
                snprintf(href,sizeof(href),"%s%s/",safe_url,enc_name);
                strbuf_appendf(&sb,"<li><a href='%s'>&#128193; <b>%s/</b></a></li>\n",href,safe_name);
            }
            rewinddir(dir_p);
            /* File */
            while ((entry=readdir(dir_p))!=NULL) {
                struct stat st; char full[MAX_PATH_LEN];
                if (strcmp(entry->d_name,".")==0||strcmp(entry->d_name,"..")==0) continue;
                snprintf(full,sizeof(full),"%s/%s",fs_path,entry->d_name);
                if (stat(full,&st)!=0||!S_ISREG(st.st_mode)) continue;
                html_encode(entry->d_name,safe_name,sizeof(safe_name));
                url_encode_name(entry->d_name,enc_name,sizeof(enc_name));
                snprintf(href,sizeof(href),"%s%s",safe_url,enc_name);
                strbuf_appendf(&sb,"<li><a href='%s'>&#128196; <i>%s</i></a><span class='size'>%lld bytes</span></li>\n",href,safe_name,(long long)st.st_size);
            }
            closedir(dir_p);
        }
    }
#endif
    strbuf_append(&sb,"</ul>\n</body></html>\n");
    if (!sb.data) { strbuf_free(&sb); return NULL; }
    return sb.data;
}

/* ---- gui phan hoi HTTP ---- */

static int send_response_str(socket_t fd, int status, const char *reason,
                              const char *content_type, const char *body) {
    char header[512]; size_t body_len=strlen(body);
    int hlen=snprintf(header,sizeof(header),
        "HTTP/1.1 %d %s\r\nConnection: close\r\nContent-Type: %s\r\nContent-Length: %zu\r\n\r\n",
        status,reason,content_type,body_len);
    if (hlen<0||(size_t)hlen>=sizeof(header)) return -1;
    if (send_all(fd,header,(size_t)hlen)<0) return -1;
    return send_all(fd,body,body_len);
}

/* ---- phuc vu noi dung file ---- */

static int serve_file(socket_t fd, const char *path) {
    FILE *fp; char header[512]; long file_size; char buf[BUFFER_SIZE]; int hlen;
    fp=fopen(path,"rb");
    if (!fp) return send_response_str(fd,404,"Not Found","text/html; charset=utf-8",
        "<!DOCTYPE html><html><head><meta charset='utf-8'></head><body>"
        "<h1>404 Khong tim thay file</h1><a href='/'>&#8592; Trang chu</a></body></html>\n");
    if (fseek(fp,0,SEEK_END)!=0) { fclose(fp); return -1; }
    file_size=ftell(fp);
    if (file_size<0) { fclose(fp); return -1; }
    if (fseek(fp,0,SEEK_SET)!=0) { fclose(fp); return -1; }
    hlen=snprintf(header,sizeof(header),
        "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: %s\r\nContent-Length: %ld\r\n\r\n",
        content_type_for(path),file_size);
    if (hlen<0||(size_t)hlen>=sizeof(header)) { fclose(fp); return -1; }
    if (send_all(fd,header,(size_t)hlen)<0) { fclose(fp); return -1; }
    while (1) {
        size_t nr=fread(buf,1,sizeof(buf),fp);
        if (nr>0 && send_all(fd,buf,nr)<0) { fclose(fp); return -1; }
        if (nr<sizeof(buf)) { if(ferror(fp)){fclose(fp);return -1;} break; }
    }
    fclose(fp); return 0;
}

/* ---- xu ly yeu cau HTTP ---- */

static void handle_request(socket_t fd) {
    char buf[BUFFER_SIZE]; char method[16], url_path[512], version[16];
    char fs_path[MAX_PATH_LEN]; io_result_t n;
    n=recv(fd,buf,(int)(sizeof(buf)-1),0);
    if (n<=0) return;
    buf[n]='\0';
    if (sscanf(buf,"%15s %511s %15s",method,url_path,version)!=3) {
        send_response_str(fd,400,"Bad Request","text/plain","400 Bad Request\n"); return;
    }
    if (strcmp(method,"GET")!=0) {
        send_response_str(fd,405,"Method Not Allowed","text/plain","405 Method Not Allowed\n"); return;
    }
    if (url_to_fs_path(url_path,fs_path,sizeof(fs_path))<0) {
        send_response_str(fd,400,"Bad Request","text/plain","400 Bad Request\n"); return;
    }
    printf("GET %s  ->  %s\n",url_path,fs_path); fflush(stdout);
    if (path_is_directory(fs_path)) {
        char *listing=generate_dir_listing(fs_path,url_path);
        if (!listing) { send_response_str(fd,500,"Internal Server Error","text/plain","500 Error\n"); return; }
        send_response_str(fd,200,"OK","text/html; charset=utf-8",listing);
        free(listing); return;
    }
    if (!path_exists(fs_path)) {
        send_response_str(fd,404,"Not Found","text/html; charset=utf-8",
            "<!DOCTYPE html><html><head><meta charset='utf-8'></head><body>"
            "<h1>404 Khong tim thay</h1><a href='/'>&#8592; Trang chu</a></body></html>\n");
        return;
    }
    serve_file(fd,fs_path);
}

/* ---- worker loop ---- */

#ifdef _WIN32
struct worker_arg { socket_t listener; int id; };
static unsigned __stdcall worker_loop(void *arg) {
    struct worker_arg *wa=(struct worker_arg*)arg;
    while (1) {
        struct sockaddr_in ca; int cl=(int)sizeof(ca);
        socket_t fd=accept(wa->listener,(struct sockaddr*)&ca,&cl);
        if (fd==INVALID_SOCKET) continue;
        handle_request(fd); close_socket(fd);
    }
    return 0;
}
#else
static void worker_loop(socket_t listener) {
    while (1) {
        struct sockaddr_in ca; socklen_t cl=(socklen_t)sizeof(ca);
        socket_t fd=accept(listener,(struct sockaddr*)&ca,&cl);
        if (fd==INVALID_SOCKET) { if(errno==EINTR) continue; continue; }
        handle_request(fd); close_socket(fd);
    }
}
#endif

/* ---- main ---- */

int main(int argc, char *argv[]) {
    socket_t listener; struct sockaddr_in addr; int port, workers, i;
    if (argc<2||argc>3) {
        fprintf(stderr,"Usage: %s <port> [workers]\n",argv[0]);
        fprintf(stderr,"  Phuc vu file tu thu muc hien tai.\n");
        return EXIT_FAILURE;
    }
    port=atoi(argv[1]); workers=(argc==3)?atoi(argv[2]):4;
    if (port<=0||port>65535) { fprintf(stderr,"port phai trong khoang 1-65535\n"); return EXIT_FAILURE; }
    if (workers<=0||workers>64) { fprintf(stderr,"workers phai trong khoang 1-64\n"); return EXIT_FAILURE; }
    if (initialize_socket_system()<0) { fprintf(stderr,"Khong khoi tao duoc socket\n"); return EXIT_FAILURE; }
    listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    if (listener==INVALID_SOCKET) { perror("socket"); cleanup_socket_system(); return EXIT_FAILURE; }
    { int opt=1; setsockopt(listener,SOL_SOCKET,SO_REUSEADDR,(const char*)&opt,(int)sizeof(opt)); }
    memset(&addr,0,sizeof(addr));
    addr.sin_family=AF_INET; addr.sin_addr.s_addr=htonl(INADDR_ANY); addr.sin_port=htons((unsigned short)port);
    if (bind(listener,(struct sockaddr*)&addr,sizeof(addr))<0) { perror("bind"); close_socket(listener); cleanup_socket_system(); return EXIT_FAILURE; }
    if (listen(listener,128)<0) { perror("listen"); close_socket(listener); cleanup_socket_system(); return EXIT_FAILURE; }
    printf("File HTTP server lang nghe cong %d\n",port);
    printf("Mo trinh duyet: http://127.0.0.1:%d/\n",port);
    printf("Thu muc goc: thu muc hien tai khi chay lenh\n");
    printf("So worker: %d\n",workers);
#ifdef _WIN32
    {
        struct worker_arg *args=(struct worker_arg*)calloc((size_t)workers,sizeof(*args));
        HANDLE *handles=(HANDLE*)calloc((size_t)workers,sizeof(HANDLE));
        if (!args||!handles){fprintf(stderr,"calloc that bai\n");free(args);free(handles);close_socket(listener);cleanup_socket_system();return EXIT_FAILURE;}
        for(i=0;i<workers;i++){unsigned tid;args[i].listener=listener;args[i].id=i+1;handles[i]=(HANDLE)_beginthreadex(NULL,0,worker_loop,&args[i],0,&tid);if(!handles[i])fprintf(stderr,"Khong tao duoc worker %d\n",i+1);}
        WaitForMultipleObjects((DWORD)workers,handles,TRUE,INFINITE);
        for(i=0;i<workers;i++){if(handles[i])CloseHandle(handles[i]);}
        free(args);free(handles);
    }
#else
    {
        signal(SIGCHLD,reap_children);
        for(i=0;i<workers;i++){pid_t child=fork();if(child<0){perror("fork");continue;}if(child==0){worker_loop(listener);exit(0);}}
        while(1) pause();
    }
#endif
    close_socket(listener); cleanup_socket_system(); return EXIT_SUCCESS;
}