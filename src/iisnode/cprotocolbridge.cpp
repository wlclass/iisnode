#include "precomp.h"

HRESULT CProtocolBridge::PostponeProcessing(CNodeHttpStoredContext* context, DWORD dueTime)
{
	CAsyncManager* async = context->GetNodeApplication()->GetApplicationManager()->GetAsyncManager();
	LARGE_INTEGER delay;
	delay.QuadPart = dueTime;

	return async->SetTimer(context->GetAsyncContext(), &delay);
}

HRESULT CProtocolBridge::SendEmptyResponse(CNodeHttpStoredContext* context, USHORT status, PCTSTR reason, HRESULT hresult)
{
	// TODO, tjanczuk, return an empty response and terminate request processing
	return S_OK;
}

HRESULT CProtocolBridge::InitiateRequest(CNodeHttpStoredContext* context)
{
	context->SetNextProcessor(CProtocolBridge::CreateNamedPipeConnection);
	CProtocolBridge::CreateNamedPipeConnection(S_OK, 0, context->GetOverlapped());

	return S_OK;
}

void WINAPI CProtocolBridge::CreateNamedPipeConnection(DWORD error, DWORD bytesTransfered, LPOVERLAPPED overlapped)
{
	HRESULT hr;
	CNodeHttpStoredContext* ctx = CNodeHttpStoredContext::Get(overlapped);
	HANDLE pipe;

	ErrorIf(INVALID_HANDLE_VALUE == (pipe = CreateFile(
		ctx->GetNodeProcess()->GetNamedPipeName(),
		GENERIC_READ | GENERIC_WRITE | FILE_FLAG_OVERLAPPED,
		0,
		NULL,
		OPEN_EXISTING,
		0,
		NULL)), 
		GetLastError());

	ctx->SetPipe(pipe);
	ctx->GetNodeApplication()->GetApplicationManager()->GetAsyncManager()->AddAsyncCompletionHandle(pipe);
	
	CProtocolBridge::SendHttpRequestHeaders(ctx);

	return;

Error:

	if (hr == ERROR_PIPE_BUSY)
	{
		DWORD retry = ctx->GetConnectionRetryCount();
		if (retry >= CModuleConfiguration::GetMaxNamedPipeConnectionRetry())
		{
			CProtocolBridge::SendEmptyResponse(ctx, 503, "Service Unavailable", hr);
		}
		else 
		{
			ctx->SetConnectionRetryCount(retry + 1);
			CProtocolBridge::PostponeProcessing(ctx, CModuleConfiguration::GetNamePipeConnectionRetryDelay());
		}
	}
	else 
	{
		CProtocolBridge::SendEmptyResponse(ctx, 500, _T("Internal Server Error"), hr);
	}

	return;
}

void CProtocolBridge::SendHttpRequestHeaders(CNodeHttpStoredContext* context)
{
	HRESULT hr;
	DWORD length;

	CheckError(CHttpProtocol::SerializeRequestHeaders(context->GetHttpContext(), context->GetBufferRef(), context->GetBufferSizeRef(), &length));

	context->SetNextProcessor(CProtocolBridge::SendHttpRequestHeadersCompleted);
	hr = WriteFile(context->GetPipe(), context->GetBuffer(), length, NULL, context->GetOverlapped()) ? S_OK : GetLastError();
	ErrorIf(hr != S_OK && hr != ERROR_IO_PENDING, hr);

	return;

Error:

	CProtocolBridge::SendEmptyResponse(context, 500, _T("Internal Server Error"), hr);

	return;
}

void WINAPI CProtocolBridge::SendHttpRequestHeadersCompleted(DWORD error, DWORD bytesTransfered, LPOVERLAPPED overlapped)
{
	CNodeHttpStoredContext* ctx = CNodeHttpStoredContext::Get(overlapped);

	if (S_OK == error)
	{
		CProtocolBridge::ReadRequestBody(ctx);
	}
	else
	{
		CProtocolBridge::SendEmptyResponse(ctx, 500, _T("Internal Server Error"), error);
	}
}

void CProtocolBridge::ReadRequestBody(CNodeHttpStoredContext* context)
{
	HRESULT hr;	
	DWORD bytesReceived;
	BOOL completionPending;

	context->SetNextProcessor(CProtocolBridge::ReadRequestBodyCompleted);
	CheckError(context->GetHttpContext()->GetRequest()->ReadEntityBody(context->GetBuffer(), context->GetBufferSize(), TRUE, &bytesReceived, &completionPending));
	if (!completionPending)
	{
		CProtocolBridge::ReadRequestBodyCompleted(S_OK, bytesReceived, context->GetOverlapped());
	}

	return;
Error:

	if (ERROR_HANDLE_EOF == hr)
	{
		CProtocolBridge::StartReadResponse(context);
	}
	else
	{
		CProtocolBridge::SendEmptyResponse(context, 500, _T("Internal Server Error"), hr);
	}

	return;
}

void WINAPI CProtocolBridge::ReadRequestBodyCompleted(DWORD error, DWORD bytesTransfered, LPOVERLAPPED overlapped)
{	
	CNodeHttpStoredContext* ctx = CNodeHttpStoredContext::Get(overlapped);

	if (S_OK == error && bytesTransfered > 0)
	{
		CProtocolBridge::SendRequestBody(ctx, bytesTransfered);
	}
	else if (ERROR_HANDLE_EOF == error)
	{		
		CProtocolBridge::StartReadResponse(ctx);
	}
	else 
	{
		CProtocolBridge::SendEmptyResponse(ctx, 500, _T("Internal Server Error"), error);
	}
}

void CProtocolBridge::SendRequestBody(CNodeHttpStoredContext* context, DWORD chunkLength)
{
	HRESULT hr;

	context->SetNextProcessor(CProtocolBridge::SendRequestBodyCompleted);
	hr = WriteFile(context->GetPipe(), context->GetBuffer(), chunkLength, NULL, context->GetOverlapped()) ? S_OK : GetLastError();
	ErrorIf(hr != S_OK && hr != ERROR_IO_PENDING, hr);

	return;

Error:

	CProtocolBridge::SendEmptyResponse(context, 500, _T("Internal Server Error"), hr);

	return;
}

void WINAPI CProtocolBridge::SendRequestBodyCompleted(DWORD error, DWORD bytesTransfered, LPOVERLAPPED overlapped)
{
	CNodeHttpStoredContext* ctx = CNodeHttpStoredContext::Get(overlapped);

	if (S_OK == error)
	{
		CProtocolBridge::ReadRequestBody(ctx);
	}
	else 
	{
		CProtocolBridge::SendEmptyResponse(ctx, 500, _T("Internal Server Error"), error);
	}
}

void CProtocolBridge::StartReadResponse(CNodeHttpStoredContext* context)
{
	context->SetDataSize(0);
	context->SetParsingOffset(0);
	context->SetNextProcessor(CProtocolBridge::ProcessResponseStatusLine);
	CProtocolBridge::ContinueReadResponse(context);
}

HRESULT CProtocolBridge::EnsureBuffer(CNodeHttpStoredContext* context)
{
	HRESULT hr;

	if (context->GetBufferSize() == context->GetDataSize())
	{
		// buffer is full

		if (context->GetParsingOffset() > 0)
		{
			// remove already parsed data from buffer by moving unprocessed data to the front; this will free up space at the end

			char* b = (char*)context->GetBuffer();
			memcpy(b, b + context->GetParsingOffset(), context->GetDataSize() - context->GetParsingOffset());
		}
		else 
		{
			// allocate more buffer memory

			DWORD* bufferLength = context->GetBufferSizeRef();
			void** buffer = context->GetBufferRef();

			DWORD quota = CModuleConfiguration::GetMaximumRequestBufferSize();
			ErrorIf(*bufferLength >= quota, ERROR_NOT_ENOUGH_QUOTA);

			void* newBuffer;
			DWORD newBufferLength = *bufferLength * 2;
			if (newBufferLength > quota)
			{
				newBufferLength = quota;
			}

			ErrorIf(NULL == (newBuffer = context->GetHttpContext()->AllocateRequestMemory(newBufferLength)), ERROR_NOT_ENOUGH_MEMORY);
			memcpy(newBuffer, (char*)(*buffer) + context->GetParsingOffset(), context->GetDataSize() - context->GetParsingOffset());
			*buffer = newBuffer;
			*bufferLength = newBufferLength;
		}

		context->SetDataSize(context->GetDataSize() - context->GetParsingOffset());
		context->SetParsingOffset(0);
	}

	return S_OK;
Error:
	return hr;

}

void CProtocolBridge::ContinueReadResponse(CNodeHttpStoredContext* context)
{
	HRESULT hr;

	CheckError(CProtocolBridge::EnsureBuffer(context));

	ErrorIf(FALSE == ReadFile(
			context->GetPipe(), 
			(char*)context->GetBuffer() + context->GetDataSize(), 
			context->GetBufferSize() - context->GetDataSize(), 
			NULL, 
			context->GetOverlapped()),
		GetLastError());

	return;
Error:

	if (ERROR_IO_PENDING != hr)
	{
		CProtocolBridge::SendEmptyResponse(context, 500, _T("Internal Server Error"), hr);
	}

	return;
}

void WINAPI CProtocolBridge::ProcessResponseStatusLine(DWORD error, DWORD bytesTransfered, LPOVERLAPPED overlapped)
{
	HRESULT hr;
	CNodeHttpStoredContext* ctx = CNodeHttpStoredContext::Get(overlapped);

	CheckError(error);
	ctx->SetDataSize(ctx->GetDataSize() + bytesTransfered);
	CheckError(CHttpProtocol::ParseResponseStatusLine(ctx));
	ctx->SetNextProcessor(CProtocolBridge::ProcessResponseHeaders);
	CProtocolBridge::ProcessResponseHeaders(S_OK, 0, ctx->GetOverlapped());

	return;
Error:

	if (ERROR_MORE_DATA == hr)
	{
		CProtocolBridge::ContinueReadResponse(ctx);
	}
	else
	{
		CProtocolBridge::SendEmptyResponse(ctx, 500, _T("Internal Server Error"), hr);
	}

	return;
}

void WINAPI CProtocolBridge::ProcessResponseHeaders(DWORD error, DWORD bytesTransfered, LPOVERLAPPED overlapped)
{
}

void WINAPI CProtocolBridge::ProcessResponseBody(DWORD error, DWORD bytesTransfered, LPOVERLAPPED overlapped)
{
}

