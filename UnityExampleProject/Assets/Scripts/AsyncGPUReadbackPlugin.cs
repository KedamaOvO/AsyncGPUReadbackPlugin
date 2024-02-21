using System;
using System.Runtime.InteropServices;
using UnityEngine.Rendering;
using UnityEngine;
using System.Collections.Generic;
using Unity.Collections;
using Unity.Collections.LowLevel.Unsafe;

namespace AsyncGPUReadbackPluginNs
{

	// Tries to match the official API
	public static class AsyncGPUReadbackPlugin
	{
		private static Queue<AsyncGPUReadbackPluginRequest> _requests = new Queue<AsyncGPUReadbackPluginRequest>();
		private static Queue<AsyncGPUReadbackPluginRequest> _requests2 = new Queue<AsyncGPUReadbackPluginRequest>();
		
		public static void RequestAsyncReadback(Texture src,Action<AsyncGPUReadbackPluginRequest> callback)
		{
			var req = new AsyncGPUReadbackPluginRequest(src,null,callback);
			_requests.Enqueue(req);
		}
		
		/// <summary>
		/// Request an async readback of a texture for a specific command buffer
		/// don't forget to call AsyncGPUReadbackPlugin.Dispose() when done
		/// </summary>
		/// <param name="cmdBuffer"></param>
		/// <param name="src"></param>
		/// <param name="callback"></param>
		/// <returns></returns>
		public static AsyncGPUReadbackPluginRequest RequestAsyncReadbackPlugin(this CommandBuffer cmdBuffer,Texture src,Action<AsyncGPUReadbackPluginRequest> callback)
		{
			var req = new AsyncGPUReadbackPluginRequest(src,cmdBuffer,callback);
			_requests.Enqueue(req);
			return req;
		}
		
		public static void RequestAsyncReadback(ComputeBuffer buffer,Action<AsyncGPUReadbackPluginRequest> callback)
		{
			var req = new AsyncGPUReadbackPluginRequest(buffer,null,callback);
			_requests.Enqueue(req);
		}
		
		/// <summary>
		/// Request an async readback of a texture for a specific command buffer
		/// don't forget to call AsyncGPUReadbackPlugin.Dispose() when done
		/// </summary>
		/// <param name="cmdBuffer"></param>
		/// <param name="src"></param>
		/// <param name="callback"></param>
		/// <returns></returns>
		public static AsyncGPUReadbackPluginRequest RequestAsyncReadbackPlugin(this CommandBuffer cmdBuffer,ComputeBuffer buffer,Action<AsyncGPUReadbackPluginRequest> callback)
		{
			var req = new AsyncGPUReadbackPluginRequest(buffer,cmdBuffer,callback);
			_requests.Enqueue(req);
			return req;
		}
		
		public static void Update()
		{
			while (_requests.Count > 0)
			{
				var req = _requests.Dequeue();
				Debug.LogFormat($"req.autoDispose:{req.autoDispose},req.done:{req.done},req.hasError:{req.hasError}");
				req.Update();
				if (req.autoDispose && (req.done || req.hasError))
				{
					req.Dispose();
				}
				else
				{
					_requests2.Enqueue(req);
				}
			}
			(_requests, _requests2) = (_requests2, _requests);
		}
	}

	public class AsyncGPUReadbackPluginRequest : IDisposable
	{
#region DllImport
		[DllImport("AsyncGPUReadbackPlugin")]
		private static extern bool isCompatible();
		[DllImport("AsyncGPUReadbackPlugin")]
		private static extern bool isSSBOCompatible();		
		[DllImport("AsyncGPUReadbackPlugin")]
		private static extern bool popRequest(int event_id);
		[DllImport("AsyncGPUReadbackPlugin")]
		private static extern bool isRequestReaded(int event_id);
		[DllImport("AsyncGPUReadbackPlugin")]
		private static extern int makeTextureRequest_mainThread(int texture, int miplevel);
		[DllImport("AsyncGPUReadbackPlugin")]
		private static extern int makeSSBORequest_mainThread(int ssbo, uint size, uint offset);
		[DllImport("AsyncGPUReadbackPlugin")]
		private static extern IntPtr getfunction_makeTextureRequest_renderThread();
        [DllImport("AsyncGPUReadbackPlugin")]
        private static extern IntPtr getfunction_makeSSBORequest_renderThread();
        [DllImport("AsyncGPUReadbackPlugin")]
		private static extern void makeTextureRequest_renderThread(int event_id);
        [DllImport("AsyncGPUReadbackPlugin")]
        private static extern void makeSSBORequest_renderThread(int event_id);
        [DllImport("AsyncGPUReadbackPlugin")]
		private static extern IntPtr getfunction_update_renderThread();
		[DllImport("AsyncGPUReadbackPlugin")]
		private static extern unsafe void getTextureData_mainThread(int event_id, ref void* buffer, ref int length);
        [DllImport("AsyncGPUReadbackPlugin")]
        private static extern unsafe void getSSBOData_mainThread(int event_id, ref void* buffer, ref int length);
        [DllImport("AsyncGPUReadbackPlugin")]
		private static extern bool isRequestError(int event_id);
		[DllImport("AsyncGPUReadbackPlugin")]
		private static extern bool isRequestDone(int event_id);
		[DllImport("AsyncGPUReadbackPlugin")]
		private static extern void dispose(int event_id);
		[DllImport("AsyncGPUReadbackPlugin")]
		private static extern void __DLL__AddDebugLogMethod(_callback_string_int_delegate callback);
#endregion

		/// <summary>
		/// Tell if we are using the plugin api or the official api
		/// </summary>
		private bool usePlugin;

		/// <summary>
		/// Event Id used to tell what texture is targeted to the render thread
		/// </summary>
		private int eventId;
		
		/// <summary>
		/// Is SSBO
		/// </summary>
		private bool isSSBO = false;
		
		/// <summary>
		/// Auto dispose the request when done
		/// </summary>
		public bool autoDispose { get; private set; }
		
		private AsyncGPUReadbackRequest _tempRequest;
		
		/// <summary>
		/// Check if the request is done
		/// </summary>
		public bool done
		{
			get
			{
				if (usePlugin)
				{
					return isRequestDone(eventId);
				}
				else
				{
					return _tempRequest.done;
				}
			}
		}
		
		/// <summary>
		/// Check if the request has an error
		/// </summary>
		public bool hasError
		{
			get
			{
				if (usePlugin)
				{
					return isRequestError(eventId);
				}
				else
				{
					return _tempRequest.hasError;
				}
			}
		}
		
		Action<AsyncGPUReadbackPluginRequest> _callback;

		static AsyncGPUReadbackPluginRequest()
		{
#if !UNITY_EDITOR && UNITY_ANDROID
			__DLL__AddDebugLogMethod(_CPP_DebugLog);
#endif
		}
        
		/// <summary>
		/// Create an AsyncGPUReadbackPluginRequest.
		/// Use official AsyncGPUReadback.Request if possible.
		/// If not, it tries to use OpenGL specific implementation
		/// Warning! Can only be called from render thread yet (not main thread)
		/// </summary>
		/// <param name="src"></param>
		/// <returns></returns>
		public AsyncGPUReadbackPluginRequest(Texture src,CommandBuffer cmdBuffer,Action<AsyncGPUReadbackPluginRequest> callback)
		{
			bool supportGPUReadBack = SystemInfo.supportsAsyncGPUReadback;
			isSSBO = false;
			Debug.LogFormat($"supportGPUReadBack:{supportGPUReadBack}");

			if (supportGPUReadBack)
			{
				usePlugin = false;
				if (cmdBuffer == null)
					AsyncGPUReadback.Request(src,0,TextureFormat.ARGB32,OnAsyncReadbackDone);
				else
					cmdBuffer.RequestAsyncReadback(src,OnAsyncReadbackDone);
				_callback = callback;
				return;
			}
			
			var flag = isCompatible();
			Debug.LogFormat($"isCompatible:{flag}");
			if (flag)
			{
				usePlugin = true;
				int textureId = (int)(src.GetNativeTexturePtr());
				this.eventId = makeTextureRequest_mainThread(textureId, 0);
				if (cmdBuffer == null)
				{
					GL.IssuePluginEvent(getfunction_makeTextureRequest_renderThread(), this.eventId);
				}
				else
				{
					cmdBuffer.IssuePluginEvent(getfunction_makeTextureRequest_renderThread(), this.eventId);
					autoDispose = false;
				}

				_callback = callback;
				return;
			}
			
			{
				Debug.LogError("AsyncGPUReadback is not supported on your system.");
			}
		}

		public AsyncGPUReadbackPluginRequest(ComputeBuffer buffer,CommandBuffer cmdBuffer,Action<AsyncGPUReadbackPluginRequest> callback)
		{
			bool supportGPUReadBack = SystemInfo.supportsAsyncGPUReadback;
			isSSBO = true;
			if (supportGPUReadBack)
			{
				usePlugin = false;
				if (cmdBuffer == null)
					AsyncGPUReadback.Request(buffer,OnAsyncReadbackDone);
				else
					cmdBuffer.RequestAsyncReadback(buffer,OnAsyncReadbackDone);
				_callback = callback;
				return;
			}
			
			var flag = isSSBOCompatible() && SystemInfo.supportsComputeShaders;
			Debug.LogFormat($"isSSBOCompatible:{flag}");
			if (flag)
			{
				// Set C++ console
				__DLL__AddDebugLogMethod(_CPP_DebugLog);

				usePlugin = true;
				int ssboId = (int)(buffer.GetNativeBufferPtr());
				this.eventId = makeSSBORequest_mainThread(ssboId, (uint)buffer.count * (uint)buffer.stride, 0);
				if (cmdBuffer == null)
				{
					GL.IssuePluginEvent(getfunction_makeSSBORequest_renderThread(), this.eventId);
				}
				else
				{
					cmdBuffer.IssuePluginEvent(getfunction_makeSSBORequest_renderThread(), this.eventId);
					
					autoDispose = false;
				}

				_callback = callback;
				return;
			}
			
			{
				Debug.LogError("AsyncGPUReadback is not supported on your system.");
			}
		}

		public unsafe NativeArray<T> GetData<T>() where T : struct
		{
			if (usePlugin)
			{
				// Get data from cpp plugin
				void* ptr = null;
				int length = 0;
				if(isSSBO)
				{
					getSSBOData_mainThread(this.eventId, ref ptr, ref length);
				}
				else
				{
					getTextureData_mainThread(this.eventId, ref ptr, ref length);
				}

				// Convert to NativeArray
				NativeArray<T> buffer = NativeArrayUnsafeUtility.ConvertExistingDataToNativeArray<T>(ptr, length / UnsafeUtility.SizeOf<T>(), Allocator.None);

				return buffer;
			}
			else
			{
				return _tempRequest.GetData<T>();
			}
		}
		
		/// <summary>
		/// Has to be called regularly to update request status.
		/// Call this from Update() or from a corountine
		/// </summary>
		/// <param name="force">Update is automatic on official api,
		/// so we don't call the Update() method except on force mode.</param>
		public void Update(bool force = false)
		{
			if (usePlugin)
			{
				GL.IssuePluginEvent(getfunction_update_renderThread(), this.eventId);

				bool readed = false;
				while (!readed)
				{
					readed = isRequestReaded(this.eventId);
					if (!readed && (this.done || this.hasError))
					{
						_callback?.Invoke(this);
						readed = true;
					}

					if (readed)
					{
						popRequest(this.eventId);
					}
				}
			}
		}
		
		private void OnAsyncReadbackDone(AsyncGPUReadbackRequest request)
		{
			_tempRequest = request;
			_callback?.Invoke(this);
		}

		/// <summary>
		/// Has to be called to free the allocated buffer after it has been used
		/// </summary>
		public void Dispose()
		{
			if (usePlugin)
			{
				dispose(this.eventId);
			}
		}

		//
		// C++から呼び出すDebug.Logのデリゲート
		delegate void _callback_string_int_delegate(string key, int val);
		[AOT.MonoPInvokeCallbackAttribute(typeof(_callback_string_int_delegate))]
		private static void _CPP_DebugLog(string key, int val)
		{
			Debug.Log("<color=cyan>[FromCPP]:" + key + ":" + val + "</color>");
		}
	}
}