#pragma once

#include <array>
#include <cstdint>

// Match the two Windows notifications by their input timestamp, independent
// of UI delivery latency. Pair occurrences instead of debouncing real repeats.
class MediaKeyDeduplicator
{
public:
	enum class Source { Hook, Hotkey };
	bool accept(unsigned key, std::uint32_t time, Source source)
	{
		if (key >= entries.size()) return false;
		for (Entry& entry : entries[key])
		{
			if (entry.valid && !entry.paired && entry.time == time && entry.source != source)
			{
				entry.paired = true;
				return false;
			}
		}
		entries[key][next[key]] = {time, source, true, false};
		next[key] = (next[key] + 1) % entries[key].size();
		return true;
	}
	void reset() { entries = {}; next = {}; }
private:
	struct Entry
	{
		std::uint32_t time = 0;
		Source source = Source::Hook;
		bool valid = false;
		bool paired = false;
	};
	std::array<std::array<Entry, 128>, 3> entries = {};
	std::array<unsigned, 3> next = {};
};
