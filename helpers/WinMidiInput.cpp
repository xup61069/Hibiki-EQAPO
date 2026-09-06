/*
    This file is part of Equalizer APO, a system-wide equalizer.
    Copyright (C) 2026
*/

#include "stdafx.h"

#include "WinMidiInput.h"

#include <algorithm>
#include <cwctype>
#include <mutex>

namespace
{
	std::wstring foldedDeviceName(const std::wstring& value)
	{
		std::wstring result = value;
		std::transform(result.begin(), result.end(), result.begin(), [](wchar_t ch) {
			return static_cast<wchar_t>(towlower(ch));
		});
		return result;
	}

	VSTMidiDeviceIdentity makeIdentity(const MIDIINCAPSW& caps, std::uint32_t ordinal)
	{
		VSTMidiDeviceIdentity identity;
		identity.manufacturerId = caps.wMid;
		identity.productId = caps.wPid;
		identity.driverVersion = caps.vDriverVersion;
		identity.nameOrdinal = ordinal;
		identity.name = caps.szPname;
		return identity;
	}

	bool identityMatches(const VSTMidiDeviceIdentity& requested, const VSTMidiDeviceIdentity& candidate)
	{
		return requested.manufacturerId == candidate.manufacturerId &&
			requested.productId == candidate.productId &&
			requested.driverVersion == candidate.driverVersion &&
			requested.nameOrdinal == candidate.nameOrdinal &&
			foldedDeviceName(requested.name) == foldedDeviceName(candidate.name);
	}

	bool stableIdentityMatches(const VSTMidiDeviceIdentity& left, const VSTMidiDeviceIdentity& right)
	{
		return left.manufacturerId == right.manufacturerId &&
			left.productId == right.productId &&
			left.nameOrdinal == right.nameOrdinal &&
			foldedDeviceName(left.name) == foldedDeviceName(right.name);
	}
}

// One process-level owner per physical WinMM input. Subscribers retain their
// own SPSC queues, so the WinMM callback only performs bounded atomic fan-out;
// opening, closing, reconnecting and subscription changes remain non-realtime.
class WinMidiInputBroker
{
public:
	static WinMidiInputBroker& instance()
	{
		static WinMidiInputBroker broker;
		return broker;
	}

	bool subscribe(const VSTMidiDeviceIdentity& identity, WinMidiInput* input)
	{
		if (input == nullptr || identity.empty())
			return false;

		std::lock_guard<std::mutex> guard(controlMutex);
		std::uint32_t sessionIndex = invalidIndex;
		for (std::uint32_t i = 0; i < sessions.size(); ++i)
		{
			// Driver versions legitimately change across reconnects. They select the
			// preferred exact device during open, but must not split one physical
			// input into two process sessions after a fallback reconnect.
			if (sessions[i].occupied && stableIdentityMatches(identity, sessions[i].identity))
			{
				sessionIndex = i;
				break;
			}
		}
		if (sessionIndex == invalidIndex)
		{
			for (std::uint32_t i = 0; i < sessions.size(); ++i)
			{
				if (!sessions[i].occupied)
				{
					sessionIndex = i;
					break;
				}
			}
		}
		if (sessionIndex == invalidIndex)
		{
			input->setConnectionStateFromBroker(VSTMidiConnectionState::BrokerCapacityExceeded);
			return false;
		}

		Session& session = sessions[sessionIndex];
		std::uint32_t subscriberIndex = invalidIndex;
		for (std::uint32_t i = 0; i < session.subscribers.size(); ++i)
		{
			if (session.subscribers[i].load(std::memory_order_acquire) == nullptr)
			{
				subscriberIndex = i;
				break;
			}
		}
		if (subscriberIndex == invalidIndex)
		{
			input->setConnectionStateFromBroker(VSTMidiConnectionState::BrokerCapacityExceeded);
			return false;
		}

		const bool createSession = !session.occupied;
		if (createSession)
		{
			session.identity = identity;
			session.occupied = true;
			session.driverClosed.store(false, std::memory_order_relaxed);
			session.state.store(VSTMidiConnectionState::Connecting, std::memory_order_relaxed);
			session.stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
			if (session.stopEvent == NULL)
			{
				resetSession(session);
				return false;
			}
		}

		session.subscribers[subscriberIndex].store(input, std::memory_order_release);
		input->brokerSessionIndex = sessionIndex;
		input->brokerSubscriberIndex = subscriberIndex;
		input->setConnectionStateFromBroker(session.state.load(std::memory_order_acquire));

		if (createSession)
		{
			session.workerThread = CreateThread(NULL, 0, workerEntry, &session, 0, NULL);
			if (session.workerThread == NULL)
			{
				session.subscribers[subscriberIndex].store(nullptr, std::memory_order_release);
				input->brokerSessionIndex = invalidIndex;
				input->brokerSubscriberIndex = invalidIndex;
				input->setConnectionStateFromBroker(VSTMidiConnectionState::Stopped);
				CloseHandle(session.stopEvent);
				session.stopEvent = NULL;
				resetSession(session);
				return false;
			}
		}
		return true;
	}

	void unsubscribe(WinMidiInput* input)
	{
		if (input == nullptr)
			return;

		std::lock_guard<std::mutex> guard(controlMutex);
		const std::uint32_t sessionIndex = input->brokerSessionIndex;
		const std::uint32_t subscriberIndex = input->brokerSubscriberIndex;
		input->brokerSessionIndex = invalidIndex;
		input->brokerSubscriberIndex = invalidIndex;
		input->setConnectionStateFromBroker(VSTMidiConnectionState::Stopped);
		if (sessionIndex >= sessions.size() || subscriberIndex >= subscriberCapacity)
			return;

		Session& session = sessions[sessionIndex];
		if (session.subscribers[subscriberIndex].exchange(nullptr, std::memory_order_acq_rel) != input)
			return;
		// A callback/worker may already have loaded the old pointer. Waiting is
		// permitted here (subscription control is explicitly non-realtime).
		while (session.dispatchReaders.load(std::memory_order_acquire) != 0)
			SwitchToThread();

		bool hasSubscribers = false;
		for (const auto& subscriber : session.subscribers)
			hasSubscribers = hasSubscribers || subscriber.load(std::memory_order_acquire) != nullptr;
		if (hasSubscribers)
			return;

		if (session.stopEvent != NULL)
			SetEvent(session.stopEvent);
		if (session.workerThread != NULL)
		{
			WaitForSingleObject(session.workerThread, INFINITE);
			CloseHandle(session.workerThread);
			session.workerThread = NULL;
		}
		if (session.stopEvent != NULL)
		{
			CloseHandle(session.stopEvent);
			session.stopEvent = NULL;
		}
		resetSession(session);
	}

private:
	static constexpr std::uint32_t sessionCapacity = 16;
	static constexpr std::uint32_t subscriberCapacity = 64;
	static constexpr std::uint32_t invalidIndex = UINT32_MAX;

	struct Session
	{
		Session()
		{
			for (auto& subscriber : subscribers)
				subscriber.store(nullptr, std::memory_order_relaxed);
		}

		WinMidiInputBroker* broker = nullptr;
		VSTMidiDeviceIdentity identity;
		std::array<std::atomic<WinMidiInput*>, subscriberCapacity> subscribers;
		std::atomic<std::uint32_t> dispatchReaders{ 0 };
		std::atomic<bool> acceptingCallbacks{ false };
		std::atomic<bool> driverClosed{ false };
		std::atomic<VSTMidiConnectionState> state{ VSTMidiConnectionState::Stopped };
		HANDLE stopEvent = NULL;
		HANDLE workerThread = NULL;
		HMIDIIN midiHandle = NULL;
		bool occupied = false;
	};

	WinMidiInputBroker()
	{
		for (Session& session : sessions)
			session.broker = this;
	}

	~WinMidiInputBroker()
	{
		for (std::uint32_t sessionIndex = 0; sessionIndex < sessions.size(); ++sessionIndex)
		{
			Session& session = sessions[sessionIndex];
			if (!session.occupied)
				continue;
			if (session.stopEvent != NULL)
				SetEvent(session.stopEvent);
			if (session.workerThread != NULL)
			{
				WaitForSingleObject(session.workerThread, INFINITE);
				CloseHandle(session.workerThread);
			}
			if (session.stopEvent != NULL)
				CloseHandle(session.stopEvent);
			for (auto& subscriber : session.subscribers)
			{
				WinMidiInput* input = subscriber.exchange(nullptr, std::memory_order_acq_rel);
				if (input != nullptr)
				{
					input->brokerSessionIndex = invalidIndex;
					input->brokerSubscriberIndex = invalidIndex;
					input->setConnectionStateFromBroker(VSTMidiConnectionState::Stopped);
				}
			}
		}
	}

	static void CALLBACK midiCallback(HMIDIIN, UINT message, DWORD_PTR instance, DWORD_PTR parameter1, DWORD_PTR parameter2)
	{
		Session* session = reinterpret_cast<Session*>(instance);
		if (session == nullptr || session->broker == nullptr)
			return;
		if ((message == MIM_DATA || message == MIM_MOREDATA) && session->acceptingCallbacks.load(std::memory_order_relaxed))
		{
			WinMidiShortMessage shortMessage;
			shortMessage.packedMessage = static_cast<std::uint32_t>(parameter1);
			shortMessage.timestampMs = static_cast<std::uint32_t>(parameter2);
			session->broker->fanOutMessage(*session, shortMessage);
		}
		else if (message == MIM_CLOSE && session->acceptingCallbacks.load(std::memory_order_relaxed))
			session->driverClosed.store(true, std::memory_order_release);
	}

	static DWORD WINAPI workerEntry(LPVOID parameter)
	{
		Session* session = static_cast<Session*>(parameter);
		return session->broker->workerLoop(*session);
	}

	DWORD workerLoop(Session& session)
	{
		while (WaitForSingleObject(session.stopEvent, 0) != WAIT_OBJECT_0)
		{
			if (session.midiHandle != NULL)
			{
				UINT currentDeviceId = 0;
				if (midiInGetID(session.midiHandle, &currentDeviceId) != MMSYSERR_NOERROR)
					session.driverClosed.store(true, std::memory_order_release);
			}
			if (session.midiHandle == NULL || session.driverClosed.exchange(false, std::memory_order_acq_rel))
			{
				closeDevice(session);
				publishState(session, VSTMidiConnectionState::Connecting);
				const MMRESULT result = openMatchingDevice(session);
				if (result == MMSYSERR_NOERROR)
					publishState(session, VSTMidiConnectionState::Connected);
				else if (result == MMSYSERR_ALLOCATED)
					publishState(session, VSTMidiConnectionState::Busy);
				else
					publishState(session, VSTMidiConnectionState::DeviceUnavailable);
			}
			if (WaitForSingleObject(session.stopEvent, WinMidiInput::reconnectIntervalMs) == WAIT_OBJECT_0)
				break;
		}
		closeDevice(session);
		publishState(session, VSTMidiConnectionState::Stopped);
		return 0;
	}

	MMRESULT openMatchingDevice(Session& session)
	{
		const std::vector<WinMidiDeviceInfo> devices = WinMidiInput::enumerateDevices();
		const WinMidiDeviceInfo* fallback = nullptr;
		MMRESULT lastResult = MMSYSERR_BADDEVICEID;
		for (const WinMidiDeviceInfo& device : devices)
		{
			if (foldedDeviceName(device.identity.name) == foldedDeviceName(session.identity.name) &&
				device.identity.manufacturerId == session.identity.manufacturerId &&
				device.identity.productId == session.identity.productId &&
				device.identity.nameOrdinal == session.identity.nameOrdinal && fallback == nullptr)
				fallback = &device;
			if (!identityMatches(session.identity, device.identity))
				continue;
			lastResult = openDevice(session, device.deviceId);
			if (lastResult == MMSYSERR_NOERROR)
				return lastResult;
		}

		if (fallback != nullptr)
			lastResult = openDevice(session, fallback->deviceId);
		return lastResult;
	}

	MMRESULT openDevice(Session& session, UINT deviceId)
	{
		HMIDIIN handle = NULL;
		const MMRESULT openResult = midiInOpen(&handle, deviceId, reinterpret_cast<DWORD_PTR>(&WinMidiInputBroker::midiCallback),
			reinterpret_cast<DWORD_PTR>(&session), CALLBACK_FUNCTION);
		if (openResult != MMSYSERR_NOERROR)
			return openResult;

		session.midiHandle = handle;
		session.acceptingCallbacks.store(true, std::memory_order_release);
		const MMRESULT startResult = midiInStart(session.midiHandle);
		if (startResult == MMSYSERR_NOERROR)
			return MMSYSERR_NOERROR;
		closeDevice(session);
		return startResult;
	}

	void closeDevice(Session& session)
	{
		session.acceptingCallbacks.store(false, std::memory_order_release);
		if (session.midiHandle == NULL)
			return;
		midiInStop(session.midiHandle);
		midiInReset(session.midiHandle);
		midiInClose(session.midiHandle);
		session.midiHandle = NULL;
	}

	void fanOutMessage(Session& session, const WinMidiShortMessage& message)
	{
		session.dispatchReaders.fetch_add(1, std::memory_order_acq_rel);
		for (const auto& subscriber : session.subscribers)
		{
			WinMidiInput* input = subscriber.load(std::memory_order_acquire);
			if (input != nullptr)
				input->publishFromBroker(message);
		}
		session.dispatchReaders.fetch_sub(1, std::memory_order_release);
	}

	void publishState(Session& session, VSTMidiConnectionState state)
	{
		session.state.store(state, std::memory_order_release);
		session.dispatchReaders.fetch_add(1, std::memory_order_acq_rel);
		for (const auto& subscriber : session.subscribers)
		{
			WinMidiInput* input = subscriber.load(std::memory_order_acquire);
			if (input != nullptr)
				input->setConnectionStateFromBroker(state);
		}
		session.dispatchReaders.fetch_sub(1, std::memory_order_release);
	}

	static void resetSession(Session& session)
	{
		session.identity = VSTMidiDeviceIdentity();
		session.acceptingCallbacks.store(false, std::memory_order_relaxed);
		session.driverClosed.store(false, std::memory_order_relaxed);
		session.state.store(VSTMidiConnectionState::Stopped, std::memory_order_relaxed);
		session.midiHandle = NULL;
		session.occupied = false;
	}

	std::mutex controlMutex;
	std::array<Session, sessionCapacity> sessions;
};

WinMidiInput::WinMidiInput()
{
}

WinMidiInput::~WinMidiInput()
{
	stop();
}

std::vector<WinMidiDeviceInfo> WinMidiInput::enumerateDevices()
{
	std::vector<WinMidiDeviceInfo> devices;
	const UINT deviceCount = midiInGetNumDevs();
	devices.reserve(deviceCount);
	for (UINT deviceId = 0; deviceId < deviceCount; ++deviceId)
	{
		MIDIINCAPSW caps = {};
		if (midiInGetDevCapsW(deviceId, &caps, sizeof(caps)) != MMSYSERR_NOERROR)
			continue;

		std::uint32_t ordinal = 0;
		const std::wstring name = foldedDeviceName(caps.szPname);
		for (const WinMidiDeviceInfo& prior : devices)
		{
			if (foldedDeviceName(prior.identity.name) == name &&
				prior.identity.manufacturerId == caps.wMid && prior.identity.productId == caps.wPid)
				++ordinal;
		}
		WinMidiDeviceInfo info;
		info.deviceId = deviceId;
		info.identity = makeIdentity(caps, ordinal);
		devices.push_back(std::move(info));
	}
	return devices;
}

bool WinMidiInput::start(const VSTMidiDeviceIdentity& identity)
{
	stop();
	if (identity.empty())
		return false;
	queue.clear();
	droppedMessages.store(0, std::memory_order_relaxed);
	state.store(VSTMidiConnectionState::Connecting, std::memory_order_release);
	if (WinMidiInputBroker::instance().subscribe(identity, this))
		return true;
	if (state.load(std::memory_order_acquire) == VSTMidiConnectionState::Connecting)
		state.store(VSTMidiConnectionState::Stopped, std::memory_order_release);
	return false;
}

void WinMidiInput::stop()
{
	if (brokerSessionIndex != UINT32_MAX)
		WinMidiInputBroker::instance().unsubscribe(this);
	else
		state.store(VSTMidiConnectionState::Stopped, std::memory_order_release);
	queue.clear();
}

bool WinMidiInput::tryPop(WinMidiShortMessage& message)
{
	return queue.tryPop(message);
}

bool WinMidiInput::ShortMessageQueue::tryPush(const WinMidiShortMessage& message)
{
	const std::uint32_t write = writeIndex.load(std::memory_order_relaxed);
	const std::uint32_t next = (write + 1) & (capacity - 1);
	if (next == readIndex.load(std::memory_order_acquire))
		return false;
	messages[write] = message;
	writeIndex.store(next, std::memory_order_release);
	return true;
}

bool WinMidiInput::ShortMessageQueue::tryPop(WinMidiShortMessage& message)
{
	const std::uint32_t read = readIndex.load(std::memory_order_relaxed);
	if (read == writeIndex.load(std::memory_order_acquire))
		return false;
	message = messages[read];
	readIndex.store((read + 1) & (capacity - 1), std::memory_order_release);
	return true;
}

void WinMidiInput::ShortMessageQueue::clear()
{
	readIndex.store(0, std::memory_order_relaxed);
	writeIndex.store(0, std::memory_order_relaxed);
}

void WinMidiInput::publishFromBroker(const WinMidiShortMessage& message)
{
	if (!queue.tryPush(message))
		droppedMessages.fetch_add(1, std::memory_order_relaxed);
}

void WinMidiInput::setConnectionStateFromBroker(VSTMidiConnectionState value)
{
	state.store(value, std::memory_order_release);
}

bool VSTMidiRuntime::configure(const std::wstring& encodedConfiguration, const std::vector<VSTParameterDescriptor>& parameters)
{
	stop();
	VSTMidiConfiguration configuration;
	if (encodedConfiguration.empty() || !VSTMidiBindingCodec::deserialize(encodedConfiguration, configuration))
		return false;

	bindings.reserve(configuration.bindings.size());
	for (const VSTMidiBinding& binding : configuration.bindings)
	{
		const bool wantsVST3 = binding.parameterType == VSTMidiParameterType::VST3ParamID;
		for (const VSTParameterDescriptor& parameter : parameters)
		{
			if (parameter.readOnly || parameter.hidden ||
				(wantsVST3 ? parameter.api != VSTParameterApi::VST3 : parameter.api != VSTParameterApi::VST2) ||
				parameter.stableId != binding.parameterId)
				continue;
			if (!wantsVST3 && parameter.name != binding.parameterName)
				continue;

			ResolvedBinding resolved;
			resolved.binding = binding;
			resolved.parameter = parameter;
			if (binding.stepCount > 0)
				resolved.parameter.stepCount = binding.stepCount;
			resolved.toggleHigh = parameter.normalizedValue >= 0.5;
			bindings.push_back(std::move(resolved));
			break;
		}
	}

	if (bindings.empty())
		return false;
	if (!input.start(configuration.device))
	{
		bindings.clear();
		return false;
	}
	return true;
}

void VSTMidiRuntime::stop()
{
	input.stop();
	bindings.clear();
	pendingUpdateCount = 0;
	pendingUpdateIndex = 0;
}

bool VSTMidiRuntime::tryPopParameterUpdate(VSTMidiParameterUpdate& update)
{
	// Bound irrelevant/system MIDI traffic per poll. Audio callers also cap
	// successful updates, so neither path can drain an unbounded producer burst.
	for (unsigned examinedMessages = 0; examinedMessages < 32; ++examinedMessages)
	{
		if (pendingUpdateIndex < pendingUpdateCount)
		{
			const PendingUpdate& pending = pendingUpdates[pendingUpdateIndex++];
			if (pending.bindingIndex >= bindings.size())
				continue;
			update.parameter = &bindings[pending.bindingIndex].parameter;
			update.normalizedValue = pending.normalizedValue;
			return true;
		}

		pendingUpdateCount = 0;
		pendingUpdateIndex = 0;
		WinMidiShortMessage message;
		if (!input.tryPop(message))
			return false;
		if (fillPendingUpdates(message))
		{
			continue;
		}
	}
	return false;
}

bool VSTMidiRuntime::fillPendingUpdates(const WinMidiShortMessage& message)
{
	const std::uint8_t status = static_cast<std::uint8_t>(message.packedMessage & 0xff);
	const std::uint8_t statusType = status & 0xf0;
	const std::uint8_t channel = status & 0x0f;
	const std::uint8_t data1 = static_cast<std::uint8_t>((message.packedMessage >> 8) & 0x7f);
	const std::uint8_t data2 = static_cast<std::uint8_t>((message.packedMessage >> 16) & 0x7f);

	VSTMidiMessageType messageType;
	double normalizedValue = 0.0;
	if (statusType == 0xb0)
	{
		messageType = VSTMidiMessageType::ControlChange;
		normalizedValue = data2 / 127.0;
	}
	else if (statusType == 0x80 || statusType == 0x90)
	{
		messageType = VSTMidiMessageType::Note;
		normalizedValue = statusType == 0x80 ? 0.0 : data2 / 127.0;
	}
	else if (statusType == 0xe0)
	{
		messageType = VSTMidiMessageType::PitchBend;
		normalizedValue = ((static_cast<unsigned>(data2) << 7) | data1) / 16383.0;
	}
	else
		return false;

	for (std::size_t i = 0; i < bindings.size() && pendingUpdateCount < pendingUpdates.size(); ++i)
	{
		ResolvedBinding& resolved = bindings[i];
		const VSTMidiBinding& binding = resolved.binding;
		if (binding.messageType != messageType || (binding.channel != 0xff && binding.channel != channel) ||
			(messageType != VSTMidiMessageType::PitchBend && binding.controlNumber != data1))
			continue;

		double outputValue = normalizedValue;
		if (binding.valueMode == VSTMidiValueMode::Toggle)
		{
			const bool high = normalizedValue >= 0.5;
			if (!high || resolved.inputHigh)
			{
				resolved.inputHigh = high;
				continue;
			}
			resolved.inputHigh = true;
			resolved.toggleHigh = !resolved.toggleHigh;
			outputValue = resolved.toggleHigh ? 1.0 : 0.0;
		}

		PendingUpdate& pending = pendingUpdates[pendingUpdateCount++];
		pending.bindingIndex = static_cast<std::uint16_t>(i);
		pending.normalizedValue = outputValue;
	}
	return pendingUpdateCount > 0;
}
