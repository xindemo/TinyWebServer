#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <pthread.h>
#include <list>
#include <exception>
#include <iostream>

#include "../locker/locker.h"
#include "../CGImysql/sql_connection_pool.h"

template <typename T>
class threadpool
{
    public:
        threadpool(int thread_number = 10, int max_requests = 10000, connection_pool* connPool = NULL);
        ~threadpool();
        bool append(T* request);

    private:
        void* worker(void* arg);
        void run();

    private:
        int m_thread_number;
        int m_max_requests;
        pthread_t* m_threads;
        std::list<T*> m_workqueue;
        mutex_lock m_quque_locker;
        sem m_ququestat;
        connection_pool *m_connPool; 
        bool m_stop;

};

template <typename T>
threadpool<T>::threadpool(int thread_number, int max_requests, connection_pool* connPool) :
    m_thread_number(thread_number), m_max_request(max_request), m_connPool(connPool), m_threads(NULL)
{
    if( (thread_number <= 0) || (max_requests <= 0) )
    {
        throw std::exception();
    }
    m_threads = new pthread_t[ m_thread_number];
    if(! m_threads)
    {
        throw std::exception();
    }

    for(int i=0; i<thread_number; ++i)
    {
        std::cout << "create the " << i << " th  thread" << std::endl;
        if(pthread_create(m_threads + i, NULL, worker, this) != 0)
        {
            delete []m_threads;
            throw std::exception();
        }
        if(pthread_detach(m_threads[i]) != 0)
        {
            delete []m_threads;
            throw std::exception();
        }
    }
}

template< typename T>
threadpool<T>::~threadpool()
{
    delete []m_threads;
}

template< typename T>
bool threadpool<T>::append( T* request)
{
    m_quque_locker.lock();
    if( m_workqueue.size() >= m_max_requests)
    {
        m_quque_locker.unlock();
        return false;
    }
    m_workqueue.push_back(request);
    m_quque_locker.unlock();
    m_ququestat.post();
    return false;
}

template<typename T>
void* threadpool<T>::worker(void *arg)
{
    threadpool *pool = (threadpool*) arg;
    pool->run();
    return pool;
}

template<typename T>
void threadpool<T>::run()
{
    while( 1)
    {
        m_ququestat.wait();
        m_quque_locker.lock();
        if(m_workqueue.empty() )
        {
            m_quque_locker.unlock();
            continue;
        }
        T* request = m_workqueue.front();
        m_quque_locker.unlock();
        if( !request)
        {
            continue;
        }
        request->process();
    }
}

#endif