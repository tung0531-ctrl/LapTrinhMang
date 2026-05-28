/*
 * calc_server.c - Bai 1: HTTP server thuc hien phep tinh cong/tru/nhan/chia
 *
 *   GET  /calc?op=add&a=5&b=3   -> trang ket qua
 *   POST /calc  (body: op=add&a=5&b=3)  -> trang ket qua
 *   GET  /   -> trang chu voi 2 form (GET va POST)
 *
 * Bien dich Linux : gcc -o calc_server calc_server.c
 * Bien dich Windows: gcc -o calc_server calc_server.c -lws2_32
 *
 * Chay: ./calc_server <port> [so_worker]
 *        Vi du: ./calc_server 8080
 */

#include <ctype.h>
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
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
typedef int         socket_t;
typedef ssize_t     io_result_t;
#define INVALID_SOCKET  (-1)
#define SOCKET_ERROR    (-1)
#define close_socket    close
#endif

#define BUFFER_SIZE 8192

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

static int send_response(socket_t fd, int status, const char *reason,
                         const char *content_type, const char *body) {
    char header[512];
    size_t body_len = strlen(body);
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Connection: close\r\n"
        "Content-Type: %s; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "\r\n",
        status, reason, content_type, body_len);
    if (hlen < 0 || (size_t)hlen >= sizeof(header)) return -1;
    if (send_all(fd, header, (size_t)hlen) < 0) return -1;
    return send_all(fd, body, body_len);
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode(const char *src, char *dst, size_t dst_size) {
    size_t i = 0;
    while (*src && i + 1 < dst_size) {
        if (*src == '%' && src[1] && src[2]) {
            int hi = hex_val(src[1]);
            int lo = hex_val(src[2]);
            if (hi >= 0 && lo >= 0) {
                dst[i++] = (char)(hi * 16 + lo);
                src += 3;
                continue;
            }
        }
        if (*src == '+') { dst[i++] = ' '; src++; continue; }
        dst[i++] = *src++;
    }
    dst[i] = '\0';
}

static void get_param(const char *query, const char *key,
                      char *out, size_t out_size) {
    char key_eq[64];
    const char *p;
    size_t key_len;
    char raw[512];
    size_t val_len;
    snprintf(key_eq, sizeof(key_eq), "%s=", key);
    key_len = strlen(key_eq);
    out[0] = '\0';
    p = query;
    while (p && *p) {
        if (strncmp(p, key_eq, key_len) == 0) {
            const char *val_start = p + key_len;
            const char *val_end   = strchr(val_start, '&');
            val_len = val_end ? (size_t)(val_end - val_start) : strlen(val_start);
            if (val_len >= sizeof(raw)) val_len = sizeof(raw) - 1;
            memcpy(raw, val_start, val_len);
            raw[val_len] = '\0';
            url_decode(raw, out, out_size);
            return;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
}

static const char HOME_PAGE[] =
    "<!DOCTYPE html>\n"
    "<html>\n"
    "<head><meta charset='utf-8'><title>May tinh HTTP</title>\n"
    "<style>\n"
    "  body{font-family:Arial,sans-serif;max-width:600px;margin:40px auto}\n"
    "  h2{color:#555;border-bottom:1px solid #ccc;padding-bottom:6px}\n"
    "  label{display:inline-block;width:90px}\n"
    "  input,select{margin:4px 0;padding:4px}\n"
    "  button{margin-top:8px;padding:6px 18px;background:#0078d4;color:#fff;border:none;cursor:pointer}\n"
    "  button:hover{background:#005ea2}\n"
    "  hr{margin:30px 0}\n"
    "</style>\n"
    "</head>\n"
    "<body>\n"
    "<h1>May tinh don gian</h1>\n"
    "<h2>Phep tinh qua GET</h2>\n"
    "<form method='GET' action='/calc'>\n"
    "  <label>So A:</label><input name='a' type='number' step='any' required/><br/>\n"
    "  <label>Toan tu:</label><select name='op'>\n"
    "    <option value='add'>Cong (+)</option>\n"
    "    <option value='sub'>Tru (-)</option>\n"
    "    <option value='mul'>Nhan (*)</option>\n"
    "    <option value='div'>Chia (/)</option>\n"
    "  </select><br/>\n"
    "  <label>So B:</label><input name='b' type='number' step='any' required/><br/>\n"
    "  <button type='submit'>Tinh (GET)</button>\n"
    "</form>\n"
    "<hr/>\n"
    "<h2>Phep tinh qua POST</h2>\n"
    "<form method='POST' action='/calc'>\n"
    "  <label>So A:</label><input name='a' type='number' step='any' required/><br/>\n"
    "  <label>Toan tu:</label><select name='op'>\n"
    "    <option value='add'>Cong (+)</option>\n"
    "    <option value='sub'>Tru (-)</option>\n"
    "    <option value='mul'>Nhan (*)</option>\n"
    "    <option value='div'>Chia (/)</option>\n"
    "  </select><br/>\n"
    "  <label>So B:</label><input name='b' type='number' step='any' required/><br/>\n"
    "  <button type='submit'>Tinh (POST)</button>\n"
    "</form>\n"
    "</body></html>\n";

static int do_calc(const char *query, char *resp_body, size_t body_size) {
    char op[16], a_str[64], b_str[64];
    double a, b, result;
    const char *op_name;
    char op_sym;
    get_param(query, "op", op,    sizeof(op));
    get_param(query, "a",  a_str, sizeof(a_str));
    get_param(query, "b",  b_str, sizeof(b_str));
    if (op[0]=='\0' || a_str[0]=='\0' || b_str[0]=='\0') {
        snprintf(resp_body, body_size,
            "<!DOCTYPE html><html><head><meta charset='utf-8'></head><body>"
            "<h1>Loi: Thieu tham so</h1><p>Can co: op, a, b.</p>"
            "<a href='/'>Quay lai</a></body></html>\n");
        return 400;
    }
    a = atof(a_str);
    b = atof(b_str);
    if      (strcmp(op,"add")==0){result=a+b;op_name="Cong"; op_sym='+';}
    else if (strcmp(op,"sub")==0){result=a-b;op_name="Tru";  op_sym='-';}
    else if (strcmp(op,"mul")==0){result=a*b;op_name="Nhan"; op_sym='*';}
    else if (strcmp(op,"div")==0){
        if(b==0.0){
            snprintf(resp_body,body_size,
                "<!DOCTYPE html><html><head><meta charset='utf-8'></head><body>"
                "<h1>Loi: Chia cho so khong</h1>"
                "<a href='/'>Quay lai</a></body></html>\n");
            return 400;
        }
        result=a/b; op_name="Chia"; op_sym='/';
    } else {
        snprintf(resp_body,body_size,
            "<!DOCTYPE html><html><head><meta charset='utf-8'></head><body>"
            "<h1>Loi: Toan tu khong hop le</h1>"
            "<p>Toan tu phai la: add, sub, mul, hoac div.</p>"
            "<a href='/'>Quay lai</a></body></html>\n");
        return 400;
    }
    snprintf(resp_body, body_size,
        "<!DOCTYPE html>\n<html>\n"
        "<head><meta charset='utf-8'><title>Ket qua</title>\n"
        "<style>body{font-family:Arial,sans-serif;max-width:500px;margin:40px auto}"
        ".res{font-size:1.5em;color:#0078d4;font-weight:bold}a{color:#0078d4}</style>\n"
        "</head><body>\n"
        "<h1>Ket qua phep tinh</h1>\n"
        "<table><tr><td><b>Phep tinh:</b></td><td>%s (%c)</td></tr>\n"
        "<tr><td><b>Toan hang A:</b></td><td>%g</td></tr>\n"
        "<tr><td><b>Toan hang B:</b></td><td>%g</td></tr>\n"
        "<tr><td><b>Bieu thuc:</b></td><td class='res'>%g %c %g = %g</td></tr>\n"
        "</table><br/><a href='/'>&#8592; Quay lai trang chu</a>\n"
        "</body></html>\n",
        op_name, op_sym, a, b, a, op_sym, b, result);
    return 200;
}

static void handle_request(socket_t fd) {
    char buf[BUFFER_SIZE];
    char method[16], url_path[512], version[16];
    char query[512];
    char resp_body[4096];
    io_result_t n;
    int status;
    n = recv(fd, buf, (int)(sizeof(buf)-1), 0);
    if (n <= 0) return;
    buf[n] = '\0';
    if (sscanf(buf, "%15s %511s %15s", method, url_path, version) != 3) {
        send_response(fd, 400, "Bad Request", "text/plain", "400 Bad Request\n");
        return;
    }
    if (strcmp(method,"GET")==0 && strcmp(url_path,"/")==0) {
        send_response(fd, 200, "OK", "text/html", HOME_PAGE);
        return;
    }
    if (strcmp(method,"GET")==0 && strncmp(url_path,"/calc",5)==0) {
        const char *q = strchr(url_path,'?');
        query[0]='\0';
        if (q) { strncpy(query,q+1,sizeof(query)-1); query[sizeof(query)-1]='\0'; }
        status = do_calc(query, resp_body, sizeof(resp_body));
        send_response(fd, status, status==200?"OK":"Bad Request", "text/html", resp_body);
        return;
    }
    if (strcmp(method,"POST")==0 && strncmp(url_path,"/calc",5)==0) {
        const char *body_start;
        const char *cl_hdr;
        int content_length = 0;
        size_t available, copy_len;
        cl_hdr = strstr(buf,"Content-Length:");
        if (!cl_hdr) cl_hdr = strstr(buf,"content-length:");
        if (cl_hdr) sscanf(cl_hdr+15,"%d",&content_length);
        body_start = strstr(buf,"\r\n\r\n");
        body_start = body_start ? body_start+4 : "";
        query[0]='\0';
        if (content_length > 0) {
            available = (size_t)(buf+n-body_start);
            copy_len  = (size_t)content_length;
            if (copy_len > available)      copy_len = available;
            if (copy_len >= sizeof(query)) copy_len = sizeof(query)-1;
            memcpy(query, body_start, copy_len);
            query[copy_len] = '\0';
        }
        status = do_calc(query, resp_body, sizeof(resp_body));
        send_response(fd, status, status==200?"OK":"Bad Request", "text/html", resp_body);
        return;
    }
    send_response(fd, 404, "Not Found", "text/plain", "404 Not Found\n");
}

#ifdef _WIN32
struct worker_arg { socket_t listener; int id; };
static unsigned __stdcall worker_loop(void *arg) {
    struct worker_arg *wa = (struct worker_arg *)arg;
    while (1) {
        struct sockaddr_in ca; int cl=(int)sizeof(ca);
        socket_t fd = accept(wa->listener,(struct sockaddr*)&ca,&cl);
        if (fd==INVALID_SOCKET) continue;
        handle_request(fd); close_socket(fd);
    }
    return 0;
}
#else
static void worker_loop(socket_t listener) {
    while (1) {
        struct sockaddr_in ca; socklen_t cl=(socklen_t)sizeof(ca);
        socket_t fd = accept(listener,(struct sockaddr*)&ca,&cl);
        if (fd==INVALID_SOCKET) { if(errno==EINTR) continue; continue; }
        handle_request(fd); close_socket(fd);
    }
}
#endif

int main(int argc, char *argv[]) {
    socket_t listener; struct sockaddr_in addr; int port, workers, i;
    if (argc<2||argc>3) { fprintf(stderr,"Usage: %s <port> [workers]\n",argv[0]); return EXIT_FAILURE; }
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
    printf("Calculator HTTP server lang nghe cong %d\n",port);
    printf("Mo trinh duyet: http://127.0.0.1:%d/\n",port);
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