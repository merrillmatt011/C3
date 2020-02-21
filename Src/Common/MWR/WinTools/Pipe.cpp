#include "Stdafx.h"
#include "Pipe.h"
#include "Common/MWR/CppTools/ScopeGuard.h"

#include "sddl.h"
#pragma comment(lib, "advapi32.lib")

namespace
{
	BOOL CreateDACL(SECURITY_ATTRIBUTES* pSA)
	{
		pSA->nLength = sizeof(SECURITY_ATTRIBUTES);
		pSA->bInheritHandle = FALSE;

		TCHAR* szSD = TEXT("D:")       // Discretionary ACL
			TEXT("(D;OICI;GA;;;BG)")     // Deny access to
										 // built-in guests
			TEXT("(D;OICI;GA;;;AN)")     // Deny access to
										 // anonymous logon
			TEXT("(A;OICI;GRGW;;;AU)")   // Allow
										 // read/write
										 // to authenticated
										 // users
			TEXT("(A;OICI;GA;;;BA)");    // Allow full control
										 // to administrators

		if (NULL == pSA)
			return FALSE;

		return ConvertStringSecurityDescriptorToSecurityDescriptor(
			szSD,
			SDDL_REVISION_1,
			&(pSA->lpSecurityDescriptor),
			NULL);
	}

	BOOL FreeDACL(SECURITY_ATTRIBUTES* pSA)
	{
		return NULL == LocalFree(pSA->lpSecurityDescriptor);
	}

	std::unique_ptr<SECURITY_ATTRIBUTES, std::function<void(SECURITY_ATTRIBUTES*)>> g_SecurityAttributes =
	{
		[]() {auto ptr = new SECURITY_ATTRIBUTES; CreateDACL(ptr); return ptr; }(),
		[](SECURITY_ATTRIBUTES* ptr) {FreeDACL(ptr);  delete ptr; }
	};
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
MWR::WinTools::WritePipe::WritePipe(ByteView pipename)
	:
	m_PipeName(OBF(R"(\\.\pipe\)") + std::string{ pipename }),
	m_Pipe([&]()
{
	auto handle = CreateNamedPipeA(m_PipeName.c_str(), PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, PIPE_UNLIMITED_INSTANCES, 512, 102400, 0, g_SecurityAttributes.get());
	if (!handle)
		throw std::runtime_error{ OBF("Creating pipe failed:") + m_PipeName };

	return handle;
}(), [](void* data)
{
	CloseHandle(data);
})
{

}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
size_t MWR::WinTools::WritePipe::Write(ByteView data)
{
	if (data.size() > std::numeric_limits<uint32_t>::max())
		throw std::runtime_error{ OBF("Write error, too much data.") };

	ConnectNamedPipe(m_Pipe.get(), nullptr);
	SCOPE_GUARD( DisconnectNamedPipe(m_Pipe.get()); );
	DWORD written;
	uint32_t len = static_cast<uint32_t>(data.size());
	WriteFile(m_Pipe.get(), &len, sizeof(len), nullptr, nullptr);
	WriteFile(m_Pipe.get(), data.data(), len, &written, nullptr);
	if (written != data.size())
		throw std::runtime_error{ OBF("Write pipe failed ") };

	FlushFileBuffers(m_Pipe.get());

	return data.size();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
MWR::WinTools::ReadPipe::ReadPipe(ByteView pipename)
	: m_PipeName(OBF(R"(\\.\pipe\)") + std::string{ pipename })
{

}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
MWR::ByteVector MWR::WinTools::ReadPipe::Read()
{
	HANDLE pipe = CreateFileA(m_PipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, g_SecurityAttributes.get(), OPEN_EXISTING, 0, NULL);
	if (pipe == INVALID_HANDLE_VALUE)
		return {};

	SCOPE_GUARD( CloseHandle(pipe); );

	DWORD chunkSize;
	uint32_t pipeBufferSize = 512u;
	uint32_t dataSize = 0u;
	if (!ReadFile(pipe, static_cast<LPVOID>(&dataSize), 4, nullptr, nullptr))
		throw std::runtime_error{ OBF("Unable to read data") };

	ByteVector buffer;
	buffer.resize(dataSize);

	DWORD read = 0;
	while (read < dataSize)
	{
		if (!ReadFile(pipe, (LPVOID)&buffer[read], static_cast<DWORD>((pipeBufferSize < (dataSize - read)) ? pipeBufferSize : (dataSize - read)), &chunkSize, nullptr) || !chunkSize)
			throw std::runtime_error{ OBF("Unable to read data") };

		read += chunkSize;
	}

	return buffer;
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
MWR::WinTools::DuplexPipe::DuplexPipe(ByteView inputPipeName, ByteView outputPipeName)
	: m_InputPipe(inputPipeName), m_OutputPipe(outputPipeName)
{

}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
MWR::ByteVector MWR::WinTools::DuplexPipe::Read()
{
	return m_InputPipe.Read();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
size_t MWR::WinTools::DuplexPipe::Write(ByteView data)
{
	return m_OutputPipe.Write(data);
}
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
MWR::WinTools::AlternatingPipe::AlternatingPipe(ByteView pipename)
	: m_PipeName(OBF("\\\\.\\pipe\\") + std::string{pipename})
	, m_Pipe([&]() {auto tmp = CreateFileA(m_PipeName.c_str(), GENERIC_READ | GENERIC_WRITE, 0, g_SecurityAttributes.get(), OPEN_EXISTING, SECURITY_SQOS_PRESENT | SECURITY_ANONYMOUS, NULL); return tmp == INVALID_HANDLE_VALUE ? nullptr : tmp; }())
	, m_Event(CreateEvent(nullptr, false, true, nullptr))
{
	if (!m_Pipe)
		throw std::runtime_error{ OBF("Couldn't open named") };

	if (!m_Event)
		throw std::runtime_error{ OBF("Couldn't create synchronization event") };
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
MWR::ByteVector MWR::WinTools::AlternatingPipe::Read()
{
	// Wait other side to read the pipe.
	if (WaitForSingleObject(m_Event.get(), 0) != WAIT_OBJECT_0)
		return{};

	// Read four bytes and find the length of the next chunk of data.
	DWORD chunkLength = 0, bytesReadCurrent = 0;
	DWORD bytesAvailable = 0;
	if (!ReadFile(m_Pipe.get(), &chunkLength, 4, &bytesReadCurrent, nullptr))
		throw std::runtime_error{ OBF("Couldn't read from Pipe: ") + std::to_string(GetLastError()) + OBF(".") };

	// Read the next chunk of data up to the length specified in the previous four bytes.
	ByteVector buffer;
	buffer.resize(chunkLength);
	for (DWORD bytesReadTotal = 0; bytesReadTotal < chunkLength; bytesReadTotal += bytesReadCurrent)
		if (!ReadFile(m_Pipe.get(), &buffer[bytesReadTotal], chunkLength - bytesReadTotal, &bytesReadCurrent, nullptr))
			throw std::runtime_error{ OBF("Couldn't read from Pipe: ") + std::to_string(GetLastError()) + OBF(".") };

	return buffer;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
size_t MWR::WinTools::AlternatingPipe::Write(ByteView data)
{
	// Write four bytes indicating the length of the next chunk of data.
	DWORD chunkLength = static_cast<DWORD>(data.size()), bytesWritten = 0;
	if (!WriteFile(m_Pipe.get(), &chunkLength, 4, &bytesWritten, nullptr))
		throw std::runtime_error{ OBF("Couldn't write to Pipe: ") + std::to_string(GetLastError()) + OBF(".") };

	// Write the chunk.
	if (!WriteFile(m_Pipe.get(), &data.front(), chunkLength, &bytesWritten, nullptr))
		throw std::runtime_error{ OBF("Couldn't write to Pipe: ") + std::to_string(GetLastError()) + OBF(".") };

	// Let Read() know that the pipe is ready to be read.
	SetEvent(m_Event.get());

	return data.size();
}
