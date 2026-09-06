#include <atomic>
#include <new>

#include <windows.h>
#include <objbase.h>
#include <olectl.h>

#include "AsioProxyDriver.h"
#include "AsioProxyIdentity.h"

namespace
{
HMODULE moduleHandle = nullptr;
std::atomic<long> serverLocks{0};
std::atomic<long> factoryInstances{0};

class AsioClassFactory final : public IClassFactory
{
public:
	AsioClassFactory()
	{
		factoryInstances.fetch_add(1, std::memory_order_relaxed);
	}

	HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override
	{
		if (object == nullptr)
			return E_POINTER;
		*object = nullptr;
		if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_IClassFactory))
		{
			*object = static_cast<IClassFactory*>(this);
			AddRef();
			return S_OK;
		}
		return E_NOINTERFACE;
	}

	ULONG STDMETHODCALLTYPE AddRef() override
	{
		return referenceCount_.fetch_add(1, std::memory_order_relaxed) + 1;
	}

	ULONG STDMETHODCALLTYPE Release() override
	{
		const ULONG remaining =
			referenceCount_.fetch_sub(1, std::memory_order_acq_rel) - 1;
		if (remaining == 0)
			delete this;
		return remaining;
	}

	HRESULT STDMETHODCALLTYPE CreateInstance(
		IUnknown* outer,
		REFIID iid,
		void** object) override
	{
		if (object == nullptr)
			return E_POINTER;
		*object = nullptr;
		if (outer != nullptr)
			return CLASS_E_NOAGGREGATION;
		auto* driver = new (std::nothrow) HibikiAsio::AsioProxyDriver(moduleHandle);
		if (driver == nullptr)
			return E_OUTOFMEMORY;
		const HRESULT result = driver->QueryInterface(iid, object);
		driver->Release();
		return result;
	}

	HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override
	{
		if (lock != FALSE)
			serverLocks.fetch_add(1, std::memory_order_relaxed);
		else
		{
			long current = serverLocks.load(std::memory_order_relaxed);
			while (current > 0 &&
				!serverLocks.compare_exchange_weak(
					current, current - 1,
					std::memory_order_acq_rel,
					std::memory_order_relaxed))
			{
			}
		}
		return S_OK;
	}

private:
	~AsioClassFactory()
	{
		factoryInstances.fetch_sub(1, std::memory_order_release);
	}

	std::atomic<ULONG> referenceCount_{1};
};

}

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void*)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		moduleHandle = instance;
		DisableThreadLibraryCalls(instance);
	}
	return TRUE;
}

extern "C" STDAPI DllGetClassObject(
	REFCLSID clsid,
	REFIID iid,
	void** object)
{
	if (object == nullptr)
		return E_POINTER;
	*object = nullptr;
	if (!InlineIsEqualGUID(clsid, HibikiAsio::kDriverClsid))
		return CLASS_E_CLASSNOTAVAILABLE;
	auto* factory = new (std::nothrow) AsioClassFactory();
	if (factory == nullptr)
		return E_OUTOFMEMORY;
	const HRESULT result = factory->QueryInterface(iid, object);
	factory->Release();
	return result;
}

extern "C" STDAPI DllCanUnloadNow()
{
	return serverLocks.load(std::memory_order_acquire) == 0 &&
		factoryInstances.load(std::memory_order_acquire) == 0 &&
		!HibikiAsio::AsioCallbackBridge::isPoisoned() &&
		HibikiAsio::AsioProxyDriver::liveInstanceCount() == 0 ? S_OK : S_FALSE;
}

extern "C" STDAPI DllRegisterServer()
{
	// Registration is installer-owned so upgrades can validate ownership and
	// roll back transactionally. regsvr32 must never overwrite foreign state.
	return SELFREG_E_CLASS;
}

extern "C" STDAPI DllUnregisterServer()
{
	// Unregistration is likewise installer-owned and ownership-aware.
	return SELFREG_E_CLASS;
}
