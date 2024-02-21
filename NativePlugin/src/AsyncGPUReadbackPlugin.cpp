#include <cstddef>
#include <map>
#include <queue>
#include <mutex>
#include <memory>
#include <cstring>
#include <algorithm>
#include "Unity/IUnityInterface.h"
#include "Unity/IUnityGraphics.h"
#include <iostream>
#include "TypeHelpers.hpp"



#define DEBUG 1
#ifdef DEBUG
    #include <fstream>
    #include <thread>
#endif


// #if UNITY_IOS || UNITY_TVOS
// #	include <OpenGLES/ES2/gl.h>
// #elif UNITY_ANDROID || UNITY_WEBGL
// #	include <GLES2/gl2.h>
// #elif UNITY_OSX
// #	include <OpenGL/gl3.h>
// #elif UNITY_WIN
// // On Windows, use gl3w to initialize and load OpenGL Core functions. In principle any other
// // library (like GLEW, GLFW etc.) can be used; here we use gl3w since it's simple and
// // straightforward.
// #	include "gl3w/gl3w.h"
// #elif UNITY_LINUX
// #	define GL_GLEXT_PROTOTYPES
// #	include <GL/gl.h>
// #endif

// #if UNITY_ANDROID
#define UNITY_ANDROID
#include <stdio.h>
#include <android/log.h>
#include <string.h>
// #endif

struct SSBOTask{
    GLuint ssbo;
    struct SubTask {
        GLsync fence;
        void* data;
        bool initialized = false;
        bool done = false;
        bool error = false;
        bool readed = false;
    };
    int size;
    int offset;
    std::queue<SubTask> subTaskQueue;
};

struct Task {
    GLuint texture;
    int miplevel;
    struct SubTask {
        GLuint fbo;
        GLuint pbo;
        GLsync fence;
        bool initialized = false;
        bool error = false;
        bool done = false;
        bool readed = false;
        void* data;
        int size;
        int height;
        int width;
        int depth;
        GLint internal_format;
    };
    std::queue<SubTask> subTaskQueue;
};

static IUnityInterfaces* unityInterfaces = NULL;
static IUnityGraphics* graphics = NULL;
static UnityGfxRenderer renderer = kUnityGfxRendererNull;

static std::map<int,std::shared_ptr<Task>> tasks;
static std::map<int,std::shared_ptr<SSBOTask>> ssbo_tasks;
static std::mutex tasks_mutex;
static std::mutex ssboTasks_mutex;
int next_event_id = 1;

static void UNITY_INTERFACE_API OnGraphicsDeviceEvent(UnityGfxDeviceEventType eventType);


template<typename T> 
bool tryGetValue(const std::map<int, T>& map, int key, T& outValue) {
    auto it = map.find(key);
    if (it != map.end()) {
        outValue = it->second;
        return true;
    }
    outValue = nullptr;
    return false;
}

typedef void (*_CPP_DebugLog)(const char*, int);

_CPP_DebugLog __UnityDebugLog;

#ifdef DEBUG
    #define _LOG(msg, level) UnityLog(msg, level)
#else
    #define _LOG(msg, level)
#endif

void UnityLog(const char* message, int level)
{
    if(__UnityDebugLog)
        __UnityDebugLog(message, level);
}

extern "C"
{
    ///
    /// [DLL]
    /// C#側のDebugLog出力処理を登録する
    ///
    void __DLL__AddDebugLogMethod( _CPP_DebugLog callback )
    {
        __UnityDebugLog = callback;
        __UnityDebugLog( "AddedDebugLog Callback", 1 );
    }

    /**
     * Unity plugin load event
     */
    void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API
        UnityPluginLoad(IUnityInterfaces* interface)
    {


        // #ifdef UNITY_ANDROID
        __android_log_print(ANDROID_LOG_DEBUG,"AppLog","1 UnityPluginLoad()  %d",glGetError());
        // #endif
    

        #ifdef DEBUG
            // logMain.open("/tmp/AsyncGPUReadbackPlugin_main.log", std::ios_base::app);
            // logRender.open("/tmp/AsyncGPUReadbackPlugin_render.log", std::ios_base::app);

            glEnable              ( GL_DEBUG_OUTPUT );

        #endif

        unityInterfaces = interface;
        graphics = unityInterfaces->Get<IUnityGraphics>();
            
        graphics->RegisterDeviceEventCallback(OnGraphicsDeviceEvent);
            
        // Run OnGraphicsDeviceEvent(initialize) manually on plugin load
        // to not miss the event in case the graphics device is already initialized
        OnGraphicsDeviceEvent(kUnityGfxDeviceEventInitialize);


        #ifdef UNITY_ANDROID
        __android_log_print(ANDROID_LOG_DEBUG,"AppLog","2 UnityPluginLoad()  %d",glGetError());
        #endif
    
    }

    /**
     * Unity unload plugin event
     */
    void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API UnityPluginUnload()
    {
        // _LOG( "UnityPluginUnload::", 0 );
        graphics->UnregisterDeviceEventCallback(OnGraphicsDeviceEvent);
        // _LOG( "UnityPluginUnload::", 1 );
    }

    /**
     * Called for every graphics device events
     */
    static void UNITY_INTERFACE_API OnGraphicsDeviceEvent(UnityGfxDeviceEventType eventType)
    {
        #ifdef UNITY_ANDROID
        __android_log_print(ANDROID_LOG_DEBUG,"AppLog","1 OnGraphicsDeviceEvent()  %d",glGetError());
        #endif
    
        // Create graphics API implementation upon initialization
        if (eventType == kUnityGfxDeviceEventInitialize)
        {
            renderer = graphics->GetRenderer();
        }

        // Cleanup graphics API implementation upon shutdown
        if (eventType == kUnityGfxDeviceEventShutdown)
        {
            renderer = kUnityGfxRendererNull;
        }

        #ifdef UNITY_ANDROID
        __android_log_print(ANDROID_LOG_DEBUG,"AppLog","2 OnGraphicsDeviceEvent()  %d",glGetError());
        #endif
    }


    /**
     * Check if plugin is compatible with this system
     * This plugin is only compatible with opengl core
     */
    bool isCompatible() {
        #ifdef UNITY_ANDROID
        __android_log_print(ANDROID_LOG_DEBUG,"AppLog","isCompatible::  %d", (int)renderer );
        #endif

        bool flag = (renderer == kUnityGfxRendererOpenGLES20 || renderer == kUnityGfxRendererOpenGLES30);

        return flag;
    }

    bool isSSBOCompatible() {
        return renderer == kUnityGfxRendererOpenGLES30;
    }

    /**
     * @brief Init of the make request action.
     * You then have to call makeRequest_renderThread
     * via GL.IssuePluginEvent with the returned event_id
     *c
     * @param ssbo OpenGL SSBO id
     * @return event_id to give to other functions and to IssuePluginEvent
     */

    int makeSSBORequest_mainThread(GLuint ssbo,uint size,uint offset) {
        _LOG( "makeSSBORequest_mainThread__:: START ", 0 );

        // Create the task
        std::shared_ptr<SSBOTask> task = std::make_shared<SSBOTask>();
        task->ssbo = ssbo;
        task->size = size;
        task->offset = offset;

        int event_id = next_event_id;
        next_event_id++;

        // Save it (lock because possible vector resize)
        ssboTasks_mutex.lock();
        ssbo_tasks[event_id] = task;
        ssboTasks_mutex.unlock();

        _LOG( "makeSSBORequest_mainThread__:: END ", 1 );

        return event_id;
    }

    /**
    @brief Create a a read ssbo request
    Has to be called by GL.IssuePluginEvent or CommandBuffer.IssuePluginEvent after DispatchCompute.
    @param event_id containing the the task index, given by makeSSBORequest_mainThread
    */
    void UNITY_INTERFACE_API makeSSBORequest_renderThread(int event_id) {
        _LOG( "makeSSBORequest_renderThread__:: START ", 0 );

        //Get task back
        ssboTasks_mutex.lock();
        std::shared_ptr<SSBOTask> task = ssbo_tasks[event_id];
        ssboTasks_mutex.unlock();

        SSBOTask::SubTask subTask;

        subTask.data = std::malloc(task->size);

        // Fence to know when it's ready
        subTask.fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        // Done init
        subTask.initialized = true;
        subTask.done = false;
        subTask.error = false;

        ssboTasks_mutex.lock();
        task->subTaskQueue.push(subTask);
        ssboTasks_mutex.unlock();

        _LOG( "makeSSBORequest_renderThread__:: END ", 1 );
    }

    /**
     * @brief Init of the make request action.
     * You then have to call makeRequest_renderThread
     * via GL.IssuePluginEvent with the returned event_id
     * 
     * @param texture OpenGL texture id
     * @return event_id to give to other functions and to IssuePluginEvent
     */
    int makeTextureRequest_mainThread(GLuint texture, int miplevel) {
        _LOG( "makeTextureRequest_mainThread__:: START ", 0 );

        // Create the task
        std::shared_ptr<Task> task = std::make_shared<Task>();
        task->texture = texture;
        task->miplevel = miplevel;
        int event_id = next_event_id;
        next_event_id++;


        // Save it (lock because possible vector resize)
        tasks_mutex.lock();
        tasks[event_id] = task;
        tasks_mutex.unlock();

        _LOG( "makeTextureRequest_mainThread__:: END ", 1 );

        return event_id;
    }

    /**
     * @brief Create a a read texture request
     * Has to be called by GL.IssuePluginEvent
     * @param event_id containing the the task index, given by makeRequest_mainThread
     */
    void UNITY_INTERFACE_API makeTextureRequest_renderThread(int event_id) {
        _LOG( "makeTextureRequest_renderThread_:: START ", 0 );

        // Get task back
        tasks_mutex.lock();
        std::shared_ptr<Task> task = tasks[event_id];
        tasks_mutex.unlock();
        Task::SubTask subTask;

        // Get texture informations
        glBindTexture(GL_TEXTURE_2D, task->texture);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, task->miplevel, GL_TEXTURE_INTERNAL_FORMAT, &(subTask.internal_format));
        glGetTexLevelParameteriv(GL_TEXTURE_2D, task->miplevel, GL_TEXTURE_DEPTH, &(subTask.depth));
        glGetTexLevelParameteriv(GL_TEXTURE_2D, task->miplevel, GL_TEXTURE_WIDTH, &(subTask.width));
        glGetTexLevelParameteriv(GL_TEXTURE_2D, task->miplevel, GL_TEXTURE_HEIGHT, &(subTask.height));
        subTask.size = subTask.depth * subTask.width * subTask.height * getPixelSizeFromInternalFormat(subTask.internal_format);


        // Check for errors
        if (subTask.size == 0
            || getFormatFromInternalFormat(subTask.internal_format) == 0
            || getTypeFromInternalFormat(subTask.internal_format) == 0) {
            subTask.error = true;
            subTask.done = true;
            GLenum error = glGetError();
            char buffer[512];
            sprintf(buffer, "makeTextureRequest_renderThread__:: ERROR(%x) %d %d %d %d %d %d %d",error,task->texture, subTask.size, subTask.internal_format,
             subTask.width, subTask.height, subTask.depth, getPixelSizeFromInternalFormat(subTask.internal_format));
            _LOG( buffer, 1 );
            return;
        }

        // Allocate the final data buffer !!! WARNING: free, will have to be done on script side !!!!
        subTask.data = std::malloc(subTask.size);

        // Create the fbo (frame buffer object) from the given texture
        // subTask.fbo;
        glGenFramebuffers(1, &(subTask.fbo));

        // Bind the texture to the fbo
        glBindFramebuffer(GL_FRAMEBUFFER, subTask.fbo);


        // glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, subTask.texture, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, task->texture, 0);
        // GL_TEXTURE_RECTANGLE, GL_TEXTURE_2D_MULTISAMPLE, or GL_TEXTURE_2D_MULTISAMPLE_ARRAY, then level must be zero.

        // Create and bind pbo (pixel buffer object) to fbo
        // subTask.pbo;
        glGenBuffers(1, &(subTask.pbo));
        glBindBuffer(GL_PIXEL_PACK_BUFFER, subTask.pbo);
        glBufferData(GL_PIXEL_PACK_BUFFER, subTask.size, 0, GL_DYNAMIC_READ);

        // Start the read request
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glReadPixels(0, 0, subTask.width, subTask.height, getFormatFromInternalFormat(subTask.internal_format), getTypeFromInternalFormat(subTask.internal_format), 0);

        // Unbind buffers
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);

        // Fence to know when it's ready
        subTask.fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        
        // Done init
        subTask.initialized = true;
        subTask.done = false;
        subTask.error = false;

        tasks_mutex.lock();
        task->subTaskQueue.push(subTask);
        tasks_mutex.unlock();

        _LOG( "makeTextureRequest_renderThread__:: END ", 1 );
    }

    UnityRenderingEvent UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API getfunction_makeTextureRequest_renderThread() {
        return makeTextureRequest_renderThread;
    }

    UnityRenderingEvent UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API getfunction_makeSSBORequest_renderThread() {
        return makeSSBORequest_renderThread;
    }

    /**
     * @brief check if data is ready
     * Has to be called by GL.IssuePluginEvent
     * @param event_id containing the the task index, given by makeRequest_mainThread
     */
    void UNITY_INTERFACE_API update_renderThread(int event_id) {
        _LOG( "update_renderThread__:: START", 0);

        // Get task back
        tasks_mutex.lock();
        std::shared_ptr<Task> task;
        bool taskExist = tryGetValue(tasks, event_id, task);
        tasks_mutex.unlock();

        // Check if task has not been already deleted by main thread
        if(taskExist && task != nullptr) {
            while(!task->subTaskQueue.empty()){
                Task::SubTask& subTask = task->subTaskQueue.front();
                if(subTask.readed){
                    tasks_mutex.lock();
                    std::free(subTask.data);
                    task->subTaskQueue.pop();
                    tasks_mutex.unlock();
                    continue;
                }

                // Do something only if initialized (thread safety)
                if (!subTask.initialized || subTask.done) {
                    break;
                }

                // Check fence state
                GLint status = 0;
                GLsizei length = 0;
                glGetSynciv(subTask.fence, GL_SYNC_STATUS, 1, &length, &status);
                if (length <= 0) {
                    subTask.error = true;
                    subTask.done = true;
                    break;
                }

                // When it's done
                if (status == GL_SIGNALED) {

                    // Bind back the pbo
                    glBindBuffer(GL_PIXEL_PACK_BUFFER, subTask.pbo);

                    // Map the buffer and copy it to data
                    void* ptr = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, subTask.size, GL_MAP_READ_BIT);
                    std::memcpy(subTask.data, ptr, subTask.size);

                    // Unmap and unbind
                    glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
                    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

                    // Clear buffers
                    glDeleteFramebuffers(1, &(subTask.fbo));
                    glDeleteBuffers(1, &(subTask.pbo));
                    glDeleteSync(subTask.fence);

                    // yeah task is done!
                    subTask.done = true;
                    subTask.readed = false;
                }
                break;
            }
        }

        //Get SSBOtask back
        ssboTasks_mutex.lock();
        std::shared_ptr<SSBOTask> ssbo_task;
        taskExist = tryGetValue(ssbo_tasks, event_id, ssbo_task);
        ssboTasks_mutex.unlock();

        if(taskExist && ssbo_task != nullptr) {
            while(!ssbo_task->subTaskQueue.empty()){
                SSBOTask::SubTask& subTask = ssbo_task->subTaskQueue.front();
                if(subTask.readed){
                    ssboTasks_mutex.lock();
                    std::free(subTask.data);
                    ssbo_task->subTaskQueue.pop();
                    ssboTasks_mutex.unlock();
                    continue;
                }

                // Do something only if initialized (thread safety)
                if (!subTask.initialized || subTask.done) {
                    break;
                }

                // Check fence state
                GLint status = 0;
                GLsizei length = 0;
                glGetSynciv(subTask.fence, GL_SYNC_STATUS, 1, &length, &status);
                if (length <= 0) {
                    subTask.error = true;
                    subTask.done = true;
                    break;
                }

                // When it's done
                if (status == GL_SIGNALED) {
                    // Bind back the ssbo
                    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo_task->ssbo);
                    // Map the buffer and copy it to data
                    void* ptr = glMapBufferRange(GL_SHADER_STORAGE_BUFFER, ssbo_task->offset, ssbo_task->size, GL_MAP_READ_BIT);

                    std::memcpy(subTask.data, ptr, ssbo_task->size);

                    // Unmap and unbind
                    glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
                    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

                    // Clear buffers
                    glDeleteSync(subTask.fence);

                    // yeah task is done!
                    subTask.done = true;
                    subTask.readed = false;
                }
                break;
            }
        }

        _LOG( "update_renderThread__:: END", 1);
    }
    
    UnityRenderingEvent UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API getfunction_update_renderThread() {
        return update_renderThread;
    }

    /**
     * @brief Get data from the main thread
     * @param event_id containing the the task index, given by makeRequest_mainThread
     */
    void getTextureData_mainThread(int event_id, void** buffer, size_t* length) {

        _LOG( "getTextureData_mainThread::START ", 0 );

        // Get task back
        tasks_mutex.lock();
        std::shared_ptr<Task> task = tasks[event_id];
        tasks_mutex.unlock();

        if(task == nullptr){
            return;
        }

        if(task->subTaskQueue.empty()){
            return;
        }

        Task::SubTask& subTask = task->subTaskQueue.front();

        // Do something only if initialized (thread safety)
        if (!subTask.done) {
            return;
        }

        // Copy the pointer. Warning: free will have to be done on script side
        *length = subTask.size;
        *buffer = subTask.data;
        subTask.readed = true;

        _LOG( "getTextureData_mainThread:: END ",1 );
    }

    /**
     * @brief Get data from the main thread
     * @param event_id containing the the task index, given by makeSSBORequest_mainThread
     */
    void getSSBOData_mainThread(int event_id, void** buffer, size_t* length) {
        _LOG( "getSSBOData_mainThread::START ", 0 );

        // Get task back
        ssboTasks_mutex.lock();
        std::shared_ptr<SSBOTask> task = ssbo_tasks[event_id];
        ssboTasks_mutex.unlock();

        if(task == nullptr){
            return;
        }

        if(task->subTaskQueue.empty()){
            return;
        }

        SSBOTask::SubTask& subTask = task->subTaskQueue.front();

        // Do something only if initialized (thread safety)
        if (!subTask.done) {
            return;
        }

        // Copy the pointer. Warning: free will have to be done on script side
        *length = task->size;
        *buffer = subTask.data;
        subTask.readed = true;

        _LOG( "getSSBOData_mainThread:: END ",1 );
    }
    

    /**
     * @brief Check if request is done
     * @param event_id containing the the task index, given by makeRequest_mainThread
     */
    bool isRequestDone(int event_id) {

        _LOG( "isRequestDone::START ", 0 );
        bool done = false;

        // Get task back
        {
            std::unique_lock<std::mutex> lock(tasks_mutex);
            std::shared_ptr<Task> task;
            if(tryGetValue(tasks, event_id, task)){
                if(!task->subTaskQueue.empty()){
                    const Task::SubTask& subTask = task->subTaskQueue.front();
                    done = subTask.done;
                }
            }
        }

        // Get SSBOtask back
        {
            std::unique_lock<std::mutex> lock(ssboTasks_mutex);
            std::shared_ptr<SSBOTask> ssbo_task;
            if(tryGetValue(ssbo_tasks, event_id, ssbo_task)){
                if(!ssbo_task->subTaskQueue.empty()){
                    const SSBOTask::SubTask& subTask = ssbo_task->subTaskQueue.front();
                    done = subTask.done;
                }
            }
        }

        _LOG( "isRequestDone::END ", 1 );
        return done;
    }

    /**
     * @brief Check if request is in error
     * @param event_id containing the the task index, given by makeRequest_mainThread
     */
    bool isRequestError(int event_id) {
        _LOG( "isRequestError::START ", 0 );
        bool error = false;
        // Get task back
        {
            std::unique_lock<std::mutex> lock(tasks_mutex);
            std::shared_ptr<Task> task;
            if(tryGetValue(tasks, event_id, task)){
                if(!task->subTaskQueue.empty()){
                    const Task::SubTask& subTask = task->subTaskQueue.front();
                    error = subTask.error;
                }
            }
        }

        // Get SSBOtask back
        {
            std::unique_lock<std::mutex> lock(ssboTasks_mutex);
            std::shared_ptr<SSBOTask> ssbo_task;
            if(tryGetValue(ssbo_tasks, event_id, ssbo_task)){
                if(!ssbo_task->subTaskQueue.empty()){
                    const SSBOTask::SubTask& subTask = ssbo_task->subTaskQueue.front();
                    error = subTask.error;
                }
            }
        }

        _LOG( "isRequestError::END ", 1 );

        return error;
    }

    bool isRequestReaded(int event_id) {
        _LOG( "isRequestReaded::START ", 0 );
        bool readed = false;
        // Get task back
        {
            std::unique_lock<std::mutex> lock(tasks_mutex);
            std::shared_ptr<Task> task;
            if(tryGetValue(tasks, event_id, task)){
                if(!task->subTaskQueue.empty()){
                    const Task::SubTask& subTask = task->subTaskQueue.front();
                    readed = subTask.readed;
                }
            }
        }

        // Get SSBOtask back
        {
            std::unique_lock<std::mutex> lock(ssboTasks_mutex);
            std::shared_ptr<SSBOTask> ssbo_task; 
            if(tryGetValue(ssbo_tasks, event_id, ssbo_task)){
                if(!ssbo_task->subTaskQueue.empty()){
                    const SSBOTask::SubTask& subTask = ssbo_task->subTaskQueue.front();
                    readed = subTask.readed;
                }
            }
        }

        _LOG( "isRequestReaded::END ", 1 );
        return readed;
    }

    bool popRequest(int event_id) {
        _LOG( "popRequest::START ", 0 );
        bool done = false;
        // Get task back
        {
            std::unique_lock<std::mutex> lock(tasks_mutex);
            std::shared_ptr<Task> task;
            if(tryGetValue(tasks, event_id, task)){
                if(!task->subTaskQueue.empty()){
                    std::free(task->subTaskQueue.front().data);
                    task->subTaskQueue.pop();
                    done = true;
                }

            }
        }

        // Get SSBOtask back
        {
            std::unique_lock<std::mutex> lock(ssboTasks_mutex);
            std::shared_ptr<SSBOTask> ssbo_task;
            if(tryGetValue(ssbo_tasks, event_id, ssbo_task)){
                if(!ssbo_task->subTaskQueue.empty()){
                    std::free(ssbo_task->subTaskQueue.front().data);
                    ssbo_task->subTaskQueue.pop();
                    done = true;
                }

            }
        }

        _LOG( "popRequest::END ", 1 );
        return done;
    }

    /**
     * @brief clear data for a frame
     * Warning : Buffer is never cleaned, it has to be cleaned from script side 
     * Has to be called by GL.IssuePluginEvent
     * @param event_id containing the the task index, given by makeRequest_mainThread
     */
    void dispose(int event_id) {
        _LOG( "dispose::START", event_id );

        // Remove from tasks
        tasks_mutex.lock();
        std::shared_ptr<Task> task;
        if(tryGetValue(tasks, event_id, task)){
            while(!task->subTaskQueue.empty()){
                const Task::SubTask& subTask = task->subTaskQueue.front();
                std::free(subTask.data);
                task->subTaskQueue.pop();
            }
            tasks.erase(event_id);
        }
        tasks_mutex.unlock();

        // Remove from ssbo_tasks
        ssboTasks_mutex.lock();
        std::shared_ptr<SSBOTask> ssbo_task;
        if(tryGetValue(ssbo_tasks, event_id, ssbo_task)){
            while(!ssbo_task->subTaskQueue.empty()){
                const SSBOTask::SubTask& subTask = ssbo_task->subTaskQueue.front();
                std::free(subTask.data);
                ssbo_task->subTaskQueue.pop();
            }
            ssbo_tasks.erase(event_id);
        }
        ssboTasks_mutex.unlock();
        _LOG( "dispose::END" , 1);
    }
}



