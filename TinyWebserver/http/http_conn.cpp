#include "http_conn.h"

int setnonblocking(int fd)
{
    int old_option = fcntl(fd, F_GETFL);
    int new_option = old_option | O_NONBLOCK;
    fcntl( fd, F_SETFL, new_option);
    return old_option;
}

void addfd(int epollfd, int sockfd, bool one_shot)
{
    epoll_event event;
    event.data.fd = epollfd;
    event.events = EPOLLIN | EPOLLET | EPOLLRDHUP;
    epoll_ctl( epollfd, EPOLL_CTL_ADD, sockfd, &event);
    setnonblocking( sockfd);
}

void removefd(int epollfd, int sockfd)
{
    epoll_ctl( epollfd, EPOLL_CTL_DEL, sockfd, 0);
    close( sockfd);
}

void modfd(int epollfd, int sockfd, int ev)
{
    epoll_event event;
    event.data.fd = sockfd;
    event.events = ev | EPOLLET | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl( epollfd, EPOLL_CTL_MOD, sockfd, &event);
}

int http_conn::m_epollfd = 0;
int http_conn::m_user_count = -1;

void http_conn::init(int sockfd, const sockaddr_in &addr)
{
    m_sockfd = sockfd;
    m_address = addr;
    addfd( m_epollfd, m_sockfd, true);
    m_user_count++;
    init();
}

void http_conn::init()
{
    memset(m_read_buffer, '\0', READ_BUFFER_SIZE);
    m_checked_idx = 0;
    m_read_idx = 0;

    m_start_line = 0;

    memset(m_write_buffer, '\0', WRITE_BUFFER_SIZE);
    m_write_idx = 0;

    CHECKSTATE m_checkstate = CHECK_STATE_REQUESTLINE;
    m_method = GET;

    memset(m_real_file, '\0', FILENAME_LEN);
    m_url = 0;
    m_version = 0;
    m_host = 0;
    m_content_length = 0;
    m_linger = false;
}

void http_conn::close_conn(bool real_close = true)
{
    if( real_close && (m_sockfd != -1) )
    {
        std::cout << "close  " << m_sockfd << std::endl;
        removefd( m_epollfd, m_sockfd);
        m_user_count--;
    }
}


http_conn::HTTP_CODE http_conn::parse_requestline(char *text)
{
    m_url = strpbrk(text, " \t");
    if( !m_url)
    {
        return BAD_REQUEST;
    }

    *m_url++ = '\0';
    char* method = text;
    if( strcasecmp(method, "GET") == 0)
    {
        m_method = GET;
    }
    else
    {
        return BAD_REQUEST;
    }

    m_url += strspn(m_url, " \t");
    m_version = strpbrk(m_url, " \t");
    if( !m_version)
    {
        return BAD_REQUEST;
    }

    *m_version++ = '\0';
    m_version += strspn(m_version, " \t");
    if( strcasecmp(m_version, "HTTP/1.1") != 0)
    {
        return BAD_REQUEST;
    }

    if( strncasecmp(m_url, "http://", 7) == 0)
    {
        m_url += 7;
        m_url = strchr( m_url, '/');
    }
    if( !m_url || m_url[0] != '/')
    {
        return BAD_REQUEST;
    }

    m_checkstate = CHECK_STATE_HEADERS;
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::parse_headers(char *text)
{
    if(text[0] == '\0')
    {
        if( m_content_length != 0)
        {
            m_checkstate = CHECK_STATE_CONTENT;
            return NO_REQUEST;
        }
        return GET_REQUEST;
    }
    else if( strncasecmp(text, "Connection:", 11) == 0)
    {
        text += 11;
        text += strspn(text, " \t");
        if( strcasecmp(text, "Keep-Alive") == 0)
        {
            m_linger = true;
        }
    }
    else if( strncasecmp(text, "Content-Length:", 15) == 0)
    {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol( text);
    }
    else if( strncasecmp(text, "Host:", 5) == 0)
    {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;
    }
    else
    {
        std::cout << "oop! unknown header : " << text << std::endl; 
    }
    return NO_REQUEST;
}

http_conn::HTTP_CODE http_conn::parse_content(char *text)
{
    if(m_read_idx >= m_checked_idx + m_content_length)
    {
        text[ m_content_length] = '\0';
        return GET_REQUEST;
    }
    return NO_REQUEST;
}

//从状态机
http_conn::LINE_STATUS http_conn::parse_line()
{
    char temp;
    for( ; m_checked_idx < m_read_idx; ++m_checked_idx)
    {
        temp = m_read_buffer[ m_checked_idx];
        if( temp == '\r')
        {
            if( m_checked_idx + 1 == m_read_idx)
            {
                return LINE_OPEN;
            }
            else if( m_read_buffer[ m_checked_idx + 1] == '\n')
            {
                m_read_buffer[ m_checked_idx++] = '\0';
                m_read_buffer[ m_checked_idx++] = '\0';
                return LINE_OK;
            }
        }
        else if( temp == '\n')
        {
            if( m_checked_idx > 0 && m_read_buffer[m_checked_idx-1] == '\r')
            {
                m_read_buffer[ m_checked_idx++] = '\0';
                m_read_buffer[ m_checked_idx++] = '\0';
                return LINE_OK;
            }
        }
        return LINE_BAD;
    }
}

//主状态机
http_conn::HTTP_CODE http_conn::process_read()
{
    http_conn::HTTP_CODE ret_code = NO_REQUEST;
    http_conn::LINE_STATUS line_status = LINE_OK;
    char* text = 0;

    while( ( (m_checkstate == CHECK_STATE_CONTENT) && (line_status == LINE_OK) )||
            ((line_status = parse_line()) == LINE_OK ) )
    {
        text = get_line();
        m_start_line = m_checked_idx; //??? 目前来看暂时没什么用处
        std::cout << "got 1 http line : " << text << std::endl;

        switch( m_checkstate)
        {
            case CHECK_STATE_REQUESTLINE:
            {
                ret_code = parse_requestline(text);
                if( ret_code == BAD_REQUEST)
                {
                    return BAD_REQUEST;
                }
                break;
            }
            case CHECK_STATE_HEADERS:
            {
                ret_code = parse_headers(text);
                if( ret_code == BAD_REQUEST)
                {
                    return BAD_REQUEST;
                }
                else if( ret_code == GET_REQUEST)
                {
                    return do_request();
                }
                break;
            }
            case CHECK_STATE_CONTENT:
            {
                ret_code = parse_content(text);
                if( ret_code == GET_REQUEST)
                {
                    return do_request();
                }
                line_status = LINE_OPEN;
                break;
            }
            default:
            {
                return INTERNAL_ERROR;
            }
        }
    }

    return NO_REQUEST;  // 如果没有进入循环，则说明这次读HTTP请求 不需要解析，返回NO_REQUEST
}

//处理解析后的GET请求，将请求的数据映射到指定的文件地址处
const char* doc_root = "/usr/bin";
http_conn::HTTP_CODE http_conn::do_request()
{
    strcpy( m_real_file, doc_root); //doc_root为服务器存放文件的根目录
    int len = strlen( doc_root);
    strncpy( m_real_file+len, m_url, FILENAME_LEN - len - 1);

    if( stat(m_url, &m_file_stat) < 0)
    {
        return NO_REQUEST;
    }

    if( !(m_file_stat.st_mode & S_IROTH))
    {
        return FORBIDDEN_REQUEST;
    }

    if( S_ISDIR( m_file_stat.st_mode) )
    {
        return BAD_REQUEST;
    }

    int fd = open( m_real_file, O_RDONLY);
    m_file_address = (char*)mmap( 0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close( fd);
    return FILE_REQUEST;
}

void http_conn::unmap()
{
    if( !m_file_address)
    {
        munmap( m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

// 将服务器响应消息依次加入 回复头
bool http_conn::add_response(const char *format, ...)
{
    if(m_write_idx >= WRITE_BUFFER_SIZE)
    {
        return false;
    }

    va_list arg_list;
    va_start( arg_list, format);
    int len = vsnprintf( m_write_buffer+m_write_idx, WRITE_BUFFER_SIZE - 1 -m_write_idx,
                format, arg_list);
    if( len >= WRITE_BUFFER_SIZE -1 - m_write_idx)
    {
        return false;
    }
    m_write_idx += len;
    va_end( arg_list);
    return true;
}

bool http_conn::add_status_line(int status, const char* title)
{
    return add_response( "%s %d %s\r\n", "HTTP/1.1", status, title);
}

bool http_conn::add_headers(int content_length)
{
    add_content_length(content_length);
    add_linger();
    add_blank_line();
}

bool http_conn::add_content_length( int content_length)
{
    return add_response("Content-Length: %d\r\n", content_length);
}

bool http_conn::add_blank_line()
{
    return add_response("%s", "\r\n");
}

bool http_conn::add_linger()
{
    return add_response("Connection: %s\r\n", (m_linger == true) ? "keep-alive" : "close");
}


const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request had bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found in this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";

bool http_conn::process_write(HTTP_CODE ret)
{
    switch( ret)
    {
        case BAD_REQUEST:
        {
            add_status_line( 400, error_400_title);
            add_headers( strlen(error_400_form));
            if( !add_content( error_400_form))
            {
                return false;
            }
            break;
        }
        case FORBIDDEN_REQUEST:
        {
            add_status_line( 403, error_403_title);
            add_headers( strlen(error_403_form));
            if( !add_content( error_403_form))
            {
                return false;
            }
            break;
        }
        case NO_RESOURCE:
        {
            add_status_line( 404, error_404_title);
            add_headers( strlen(error_404_form));
            if( !add_content(error_404_form))
            {
                return false;
            }
            break;
        }
        case INTERNAL_ERROR:
        {
            add_status_line( 500, error_500_title);
            add_headers( strlen(error_500_form));
            if( !add_content(error_500_form))
            {
                return false;
            }
            break;
        }
        case FILE_REQUEST:
        {
            add_status_line( 200, ok_200_title);
            if( m_file_stat.st_size != 0)
            {
                add_headers( m_file_stat.st_size);
                m_iv[0].iov_base = m_write_buffer;
                m_iv[0].iov_len = m_write_idx;
                m_iv[1].iov_base = m_file_address;
                m_iv[1].iov_len = m_file_stat.st_size;
                m_iv_count = 2;
                return true;
            }
            else
            {
                const char* ok_string = "<html><body></body></html>";
                add_headers( strlen(ok_string));
                if( !add_content( ok_string))
                {
                    return false;
                }
            }
        }
        default:
        {
            return false;
        }
    }
    m_iv[0].iov_base = m_write_buffer;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;
    return true;
}

//  该函数为线程池的worker函数调用的 入口函数，完成一个完整的HTTP通信过程
void http_conn::process()
{
    HTTP_CODE read_ret = process_read();
    if( read_ret == NO_REQUEST )
    {
        modfd( m_epollfd, m_sockfd, EPOLLIN);
        return;
    }

    bool write_ret = process_write( read_ret);
    if( !write_ret)
    {
        close_conn();
    }
    modfd( m_epollfd, m_sockfd, EPOLLOUT);
}


bool http_conn::read()
{
    if( m_read_idx >= READ_BUFFER_SIZE)
    {
        return false;
    }

    int bytes_read = 0;
    while(true)
    {
        bytes_read = recv( m_sockfd, m_read_buffer+m_read_idx, 
                        READ_BUFFER_SIZE - m_read_idx, 0);
        if( bytes_read == -1)
        {
            if( errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            return false;
        }
        else if( bytes_read == 0)
        {
            return false;
        }
        m_read_idx += bytes_read;
    }
    return true;
}

//使用writev将一个或几个文件描述符内的数据 集中写 到一起
bool http_conn::write()
{
    int temp = 0;
    int bytes_to_send = m_write_idx;
    int bytes_have_send = 0;

    if( bytes_to_send == 0)
    {
        modfd( m_epollfd, m_sockfd, EPOLLIN);
        return true;
    }

    while( 1)
    {
        temp = writev(m_sockfd, m_iv, m_iv_count);
        if( temp <= -1)
        {
            if( errno == EAGAIN)
            {
                modfd( m_epollfd, m_sockfd, EPOLLOUT);
                return true;
            }
            unmap();
            return false;
        }

        bytes_have_send += temp;
        bytes_to_send -= temp;
        if(bytes_have_send <= bytes_to_send)
        {
            unmap();
            if( m_linger)
            {
                init();
                modfd( m_epollfd, m_sockfd, EPOLLIN);
                return true;
            }
            else
            {
                modfd( m_epollfd, m_sockfd, EPOLLIN);
                return false;
            }
        }
    }
}