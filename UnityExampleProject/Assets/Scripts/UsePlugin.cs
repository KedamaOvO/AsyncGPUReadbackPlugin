using UnityEngine;
using UnityEngine.UI;
using System.Collections;
using System.Collections.Generic;
using System;
using System.Runtime.InteropServices;
using Unity.Collections;
using System.IO;
using AsyncGPUReadbackPluginNs;
using UnityEngine.Experimental.UIElements;
using UnityEngine.Rendering;

/// <summary>
/// Exemple of usage inspirated from https://github.com/keijiro/AsyncCaptureTest/blob/master/Assets/AsyncCapture.cs
/// </summary>
public class UsePlugin : MonoBehaviour
{
	[SerializeField] ComputeShader _computeShader;
	[SerializeField] RawImage _uiImage;
	Camera _camera;
	private Texture2D _texture;
	private RenderTexture _renderTexture;

	private ComputeBuffer _computeBuffer;
	private AsyncGPUReadbackPluginRequest cameraTargetRequest;
	private AsyncGPUReadbackPluginRequest computeRequest;
	private float timeFromCs;

	bool _inited = false;

	private void Awake()
	{
		_camera = GetComponent<Camera>();
		_computeBuffer = new ComputeBuffer(1, 4);
	}

	void Start()
	{
		Invoke("_Init", 1f);
	}

	private void OnDestroy()
	{
		cameraTargetRequest.Dispose();
	}

	void _Init()
	{
		_inited = true;
		_texture = new Texture2D(Screen.width,  Screen.height, TextureFormat.RGBA32, false);
		_renderTexture = new RenderTexture(Screen.width,  Screen.height, 0);
		_renderTexture.Create();
		_uiImage.texture = _texture;
		
		var commandBuffer = new CommandBuffer();
		commandBuffer.Blit(BuiltinRenderTextureType.CameraTarget, _renderTexture,new Vector2(1, -1), new Vector2(0, 1));
		cameraTargetRequest = commandBuffer.RequestAsyncReadbackPlugin(_renderTexture, req =>
		{
			if (req.done)
			{
				SaveBitmap(req.GetData<byte>(), Screen.width, Screen.height);
			}
		});
		commandBuffer.SetGlobalBuffer(Shader.PropertyToID("_Result"), _computeBuffer);
		commandBuffer.DispatchCompute(_computeShader, 0, 1, 1, 1);
		commandBuffer.RequestAsyncReadbackPlugin(_computeBuffer, req =>
		{
			if (req.done)
			{
				float time =  req.GetData<float>()[0];
				timeFromCs = time;
			}
		});
		_camera.AddCommandBuffer(CameraEvent.AfterEverything,commandBuffer);
	}

	void Update()
	{
		if(!_inited)return;
		_computeShader.SetFloat(Shader.PropertyToID("_Time"),Time.time);
		AsyncGPUReadbackPlugin.Update();
	}

	// void OnRenderImage(RenderTexture source, RenderTexture destination)
	// {
	// 	Graphics.Blit(source, destination);
	// 	if(!_inited) return;
	// 	
	// 	AsyncGPUReadbackPlugin.RequestAsyncReadback(source, req =>
	// 	{
	// 		if (req.done)
	// 		{
	// 			SaveBitmap(req.GetData<byte>(), source.width, source.height);
	// 		}
	// 	});
	// }

	private void OnGUI()
	{
		GUI.Label(new Rect(10, 10, 200, 20), $"Time from CS : {timeFromCs}");
	}

	void SaveBitmap(NativeArray<byte> buffer, int width, int height)
	{
		Debug.Log($"Write to file : {buffer.Length}");
	
		// if(_renderTexture.width != width || _renderTexture.height != height)
		// {
		// 	Debug.Log($"Resize RenderTexture : {width}x{height}");
		// 	_renderTexture.Release();
		// 	_renderTexture.width = width;
		// 	_renderTexture.height = height;
		// 	_renderTexture.Create();
		// }
		// var tex = new Texture2D(width, height, TextureFormat.RGBAHalf, false);
		if (_texture.width != width || _texture.height != height)
		{
			_texture.Resize(width, height);
		}
		_texture.LoadRawTextureData(buffer);
		_texture.Apply();
	}
}
