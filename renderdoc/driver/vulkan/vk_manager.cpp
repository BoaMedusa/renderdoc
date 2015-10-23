/******************************************************************************
 * The MIT License (MIT)
 * 
 * Copyright (c) 2015 Baldur Karlsson
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#include "vk_manager.h"
#include "vk_core.h"

template<>
void Serialiser::Serialise(const char *name, ImageRegionState &el)
{
	ScopedContext scope(this, name, "ImageRegionState", 0, true);
	
	Serialise("range", el.range);
	Serialise("prevstate", el.prevstate);
	Serialise("state", el.state);
}

bool VulkanResourceManager::SerialisableResource(ResourceId id, VkResourceRecord *record)
{
	if(id == m_Core->GetContextResourceID())
		return false;
	return true;
}

// debugging logging for transitions
#if 0
#define TRDBG(...) RDCLOG(__VA_ARGS__)
#else
#define TRDBG(...)
#endif

void VulkanResourceManager::RecordTransitions(vector< pair<ResourceId, ImageRegionState> > &trans, map<ResourceId, ImageLayouts> &states,
											  uint32_t numTransitions, const VkImageMemoryBarrier *transitions)
{
	TRDBG("Recording %u transitions", numTransitions);

	for(uint32_t ti=0; ti < numTransitions; ti++)
	{
		const VkImageMemoryBarrier &t = transitions[ti];
		
		ResourceId id = m_State < WRITING ? GetNonDispWrapper(t.image)->id : GetResID(t.image);
		
		uint32_t nummips = t.subresourceRange.mipLevels;
		uint32_t numslices = t.subresourceRange.arraySize;
		if(nummips == VK_REMAINING_MIP_LEVELS) nummips = states[id].mipLevels - t.subresourceRange.baseMipLevel;
		if(numslices == VK_REMAINING_ARRAY_LAYERS) numslices = states[id].arraySize - t.subresourceRange.baseArrayLayer;

		bool done = false;

		auto it = trans.begin();
		for(; it != trans.end(); ++it)
		{
			// image transitions are handled by initially inserting one subresource range for each aspect,
			// and whenever we need more fine-grained detail we split it immediately for one range for
			// each subresource in that aspect. Thereafter if a transition comes in that covers multiple
			// subresources, we transition all matching ranges.

			// find the transitions matching this id
			if(it->first < id) continue;
			if(it->first != id) break;

			if(it->second.range.aspectMask & t.subresourceRange.aspectMask)
			{
				// we've found a range that completely matches our region, doesn't matter if that's
				// a whole image and the transition is the whole image, or it's one subresource.
				// note that for images with only one array/mip slice (e.g. render targets) we'll never
				// really have to worry about the else{} branch
				if(it->second.range.baseMipLevel == t.subresourceRange.baseMipLevel &&
				   it->second.range.mipLevels == nummips &&
				   it->second.range.baseArrayLayer == t.subresourceRange.baseArrayLayer &&
				   it->second.range.arraySize == numslices)
				{
					// verify
					//RDCASSERT(it->second.state == t.oldState);

					// apply it (prevstate is from the start of all transitions, so only set once)
					if(it->second.prevstate == UNTRANSITIONED_IMG_STATE)
						it->second.prevstate = t.oldLayout;
					it->second.state = t.newLayout;

					done = true;
					break;
				}
				else
				{
					// this handles the case where the transition covers a number of subresources and we need
					// to transition each matching subresource. If the transition was only one mip & array slice
					// it would have hit the case above. Find each subresource within the range, transition it,
					// and continue (marking as done so whenever we stop finding matching ranges, we are
					// satisfied.
					//
					// note that regardless of how we lay out our subresources (slice-major or mip-major) the new
					// range could be sparse, but that's OK as we only break out of the loop once we go past the whole
					// aspect. Any subresources that don't match the range, after the split, will fail to meet any
					// of the handled cases, so we'll just continue processing.
					if(it->second.range.mipLevels == 1 &&
					   it->second.range.arraySize == 1 &&
					   it->second.range.baseMipLevel >= t.subresourceRange.baseMipLevel &&
					   it->second.range.baseMipLevel < t.subresourceRange.baseMipLevel+nummips &&
					   it->second.range.baseArrayLayer >= t.subresourceRange.baseArrayLayer &&
					   it->second.range.baseArrayLayer < t.subresourceRange.baseArrayLayer+numslices)
					{
						// apply it (prevstate is from the start of all transitions, so only set once)
						if(it->second.prevstate == UNTRANSITIONED_IMG_STATE)
							it->second.prevstate = t.oldLayout;
						it->second.state = t.newLayout;

						// continue as there might be more, but we're done
						done = true;
						continue;
					}
					// finally handle the case where we have a range that covers a whole image but we need to
					// split it. If the transition covered the whole image too it would have hit the very first
					// case, so we know that the transition doesn't cover the whole range.
					// Also, if we've already done the split this case won't be hit and we'll either fall into
					// the case above, or we'll finish as we've covered the whole transition.
					else if(it->second.range.mipLevels > 1 || it->second.range.arraySize > 1)
					{
						pair<ResourceId, ImageRegionState> existing = *it;

						// remember where we were in the array, as after this iterators will be
						// invalidated.
						size_t offs = it - trans.begin();
						size_t count = it->second.range.mipLevels * it->second.range.arraySize;

						// only insert count-1 as we want count entries total - one per subresource
						trans.insert(it, count-1, existing);

						// it now points at the first subresource, but we need to modify the ranges
						// to be valid
						it = trans.begin()+offs;

						for(size_t i=0; i < count; i++)
						{
							it->second.range.mipLevels = 1;
							it->second.range.arraySize = 1;

							// slice-major
							it->second.range.baseArrayLayer = uint32_t(i / existing.second.range.mipLevels);
							it->second.range.baseMipLevel = uint32_t(i % existing.second.range.mipLevels);
							it++;
						}
						
						// reset the iterator to point to the first subresource
						it = trans.begin()+offs;

						// the loop will continue after this point and look at the next subresources
						// so we need to check to see if the first subresource lies in the range here
						if(it->second.range.baseMipLevel >= t.subresourceRange.baseMipLevel &&
						   it->second.range.baseMipLevel < t.subresourceRange.baseMipLevel+nummips &&
						   it->second.range.baseArrayLayer >= t.subresourceRange.baseArrayLayer &&
						   it->second.range.baseArrayLayer < t.subresourceRange.baseArrayLayer+numslices)
						{
							// apply it (prevstate is from the start of all transitions, so only set once)
							if(it->second.prevstate == UNTRANSITIONED_IMG_STATE)
								it->second.prevstate = t.oldLayout;
							it->second.state = t.newLayout;

							// continue as there might be more, but we're done
							done = true;
						}

						// continue processing from here
						continue;
					}
				}
			}

			// if we've gone past where the new subresource range would sit
			if(it->second.range.aspectMask > t.subresourceRange.aspectMask)
				break;

			// otherwise continue to try and find the subresource range
		}

		if(done) continue;

		// we don't have an existing transition for this memory region, insert into place. it points to
		// where it should be inserted
		trans.insert(it, std::make_pair(id, ImageRegionState(t.subresourceRange, t.oldLayout, t.newLayout)));
	}

	TRDBG("Post-record, there are %u transitions", (uint32_t)trans.size());
}

void VulkanResourceManager::SerialiseImageStates(map<ResourceId, ImageLayouts> &states, vector<VkImageMemoryBarrier> &transitions)
{
	Serialiser *localSerialiser = m_pSerialiser;

	SERIALISE_ELEMENT(uint32_t, NumMems, (uint32_t)states.size());

	auto srcit = states.begin();

	vector< pair<ResourceId, ImageRegionState> > vec;

	for(uint32_t i=0; i < NumMems; i++)
	{
		SERIALISE_ELEMENT(ResourceId, id, srcit->first);
		SERIALISE_ELEMENT(uint32_t, NumStates, (uint32_t)srcit->second.subresourceStates.size());

		ResourceId liveid;
		if(m_State < WRITING && HasLiveResource(id))
			liveid = GetLiveID(id);
		auto dstit = states.find(id);

		for(uint32_t m=0; m < NumStates; m++)
		{
			SERIALISE_ELEMENT(ImageRegionState, state, srcit->second.subresourceStates[m]);

			if(m_State < WRITING && liveid != ResourceId() && srcit != states.end())
			{
				VkImageMemoryBarrier t;
				t.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
				t.pNext = NULL;
				// VKTODOHIGH losing information? what are in/out mask and queues
				// if input/output mask are just same as memory barriers, for
				// the memory bound to the image, it's maybe fine as we don't need
				// those for this purpose, they were replayed when the non-collapsed
				// barrier happened.
				t.inputMask = 0;
				t.outputMask = 0;
				t.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				t.destQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
				t.image = Unwrap(GetCurrentHandle<VkImage>(liveid));
				t.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
				t.newLayout = state.state;
				t.subresourceRange = state.range;
				transitions.push_back(t);
				vec.push_back(std::make_pair(liveid, state));
			}
		}

		if(m_State >= WRITING) srcit++;
	}

	ApplyTransitions(vec, states);

	for(size_t i=0; i < vec.size(); i++)
		transitions[i].oldLayout = vec[i].second.prevstate;

	// erase any do-nothing transitions
	for(auto it=transitions.begin(); it != transitions.end();)
	{
		if(it->oldLayout == it->newLayout)
			it = transitions.erase(it);
		else
			++it;
	}
}

void VulkanResourceManager::ApplyTransitions(vector< pair<ResourceId, ImageRegionState> > &trans, map<ResourceId, ImageLayouts> &states)
{
	TRDBG("Applying %u transitions", (uint32_t)trans.size());

	for(size_t ti=0; ti < trans.size(); ti++)
	{
		ResourceId id = trans[ti].first;
		ImageRegionState &t = trans[ti].second;

		TRDBG("Applying transition to %llu", GetOriginalID(id));

		auto stit = states.find(id);

		if(stit == states.end())
		{
			TRDBG("Didn't find ID in image states");
			continue;
		}

		uint32_t nummips = t.range.mipLevels;
		uint32_t numslices = t.range.arraySize;
		if(nummips == VK_REMAINING_MIP_LEVELS) nummips = states[id].mipLevels;
		if(numslices == VK_REMAINING_ARRAY_LAYERS) numslices = states[id].arraySize;

		// VKTODOHIGH check, does this mean the sensible thing?
		if(nummips == 0) nummips = 1;
		if(numslices == 0) numslices = 1;

		if(t.prevstate == t.state) continue;

		TRDBG("Transition of %s (%u->%u, %u->%u) from %s to %s",
				ToStr::Get(t.range.aspect).c_str(),
				t.range.baseMipLevel, t.range.mipLevels,
				t.range.baseArrayLayer, t.range.arraySize,
				ToStr::Get(t.prevstate).c_str(), ToStr::Get(t.state).c_str());

		bool done = false;

		TRDBG("Matching image has %u subresource states", stit->second.subresourceStates.size());

		auto it = stit->second.subresourceStates.begin();
		for(; it != stit->second.subresourceStates.end(); ++it)
		{
			TRDBG(".. state %s (%u->%u, %u->%u) from %s to %s",
				ToStr::Get(it->range.aspect).c_str(),
				it->range.baseMipLevel, it->range.mipLevels,
				it->range.baseArrayLayer, it->range.arraySize,
				ToStr::Get(it->prevstate).c_str(), ToStr::Get(it->state).c_str());

			// image transitions are handled by initially inserting one subresource range for each aspect,
			// and whenever we need more fine-grained detail we split it immediately for one range for
			// each subresource in that aspect. Thereafter if a transition comes in that covers multiple
			// subresources, we transition all matching ranges.

			if(it->range.aspectMask & t.range.aspectMask)
			{
				// we've found a range that completely matches our region, doesn't matter if that's
				// a whole image and the transition is the whole image, or it's one subresource.
				// note that for images with only one array/mip slice (e.g. render targets) we'll never
				// really have to worry about the else{} branch
				if(it->range.baseMipLevel == t.range.baseMipLevel &&
				   it->range.mipLevels == nummips &&
				   it->range.baseArrayLayer == t.range.baseArrayLayer &&
				   it->range.arraySize == numslices)
				{
					/*
					RDCASSERT(t.prevstate == UNTRANSITIONED_IMG_STATE || it->state == UNTRANSITIONED_IMG_STATE || // renderdoc untracked/ignored
					          it->state == t.prevstate || // valid transition
										t.prevstate == VK_IMAGE_LAYOUT_UNDEFINED); // can transition from UNDEFINED to any state
					*/
					t.prevstate = it->state;
					it->state = t.state;

					done = true;
					break;
				}
				else
				{
					// this handles the case where the transition covers a number of subresources and we need
					// to transition each matching subresource. If the transition was only one mip & array slice
					// it would have hit the case above. Find each subresource within the range, transition it,
					// and continue (marking as done so whenever we stop finding matching ranges, we are
					// satisfied.
					//
					// note that regardless of how we lay out our subresources (slice-major or mip-major) the new
					// range could be sparse, but that's OK as we only break out of the loop once we go past the whole
					// aspect. Any subresources that don't match the range, after the split, will fail to meet any
					// of the handled cases, so we'll just continue processing.
					if(it->range.mipLevels == 1 &&
					   it->range.arraySize == 1 &&
					   it->range.baseMipLevel >= t.range.baseMipLevel &&
					   it->range.baseMipLevel < t.range.baseMipLevel+nummips &&
					   it->range.baseArrayLayer >= t.range.baseArrayLayer &&
					   it->range.baseArrayLayer < t.range.baseArrayLayer+numslices)
					{
						// apply it (prevstate is from the start of all transitions, so only set once)
						if(it->prevstate == UNTRANSITIONED_IMG_STATE)
							it->prevstate = t.prevstate;
						it->state = t.state;

						// continue as there might be more, but we're done
						done = true;
						continue;
					}
					// finally handle the case where we have a range that covers a whole image but we need to
					// split it. If the transition covered the whole image too it would have hit the very first
					// case, so we know that the transition doesn't cover the whole range.
					// Also, if we've already done the split this case won't be hit and we'll either fall into
					// the case above, or we'll finish as we've covered the whole transition.
					else if(it->range.mipLevels > 1 || it->range.arraySize > 1)
					{
						ImageRegionState existing = *it;

						// remember where we were in the array, as after this iterators will be
						// invalidated.
						size_t offs = it - stit->second.subresourceStates.begin();
						size_t count = it->range.mipLevels * it->range.arraySize;

						// only insert count-1 as we want count entries total - one per subresource
						stit->second.subresourceStates.insert(it, count-1, existing);

						// it now points at the first subresource, but we need to modify the ranges
						// to be valid
						it = stit->second.subresourceStates.begin()+offs;

						for(size_t i=0; i < count; i++)
						{
							it->range.mipLevels = 1;
							it->range.arraySize = 1;

							// slice-major
							it->range.baseArrayLayer = uint32_t(i / existing.range.mipLevels);
							it->range.baseMipLevel = uint32_t(i % existing.range.mipLevels);
							it++;
						}
						
						// reset the iterator to point to the first subresource
						it = stit->second.subresourceStates.begin()+offs;

						// the loop will continue after this point and look at the next subresources
						// so we need to check to see if the first subresource lies in the range here
						if(it->range.baseMipLevel >= t.range.baseMipLevel &&
						   it->range.baseMipLevel < t.range.baseMipLevel+nummips &&
						   it->range.baseArrayLayer >= t.range.baseArrayLayer &&
						   it->range.baseArrayLayer < t.range.baseArrayLayer+numslices)
						{
							// apply it (prevstate is from the start of all transitions, so only set once)
							if(it->prevstate == UNTRANSITIONED_IMG_STATE)
								it->prevstate = t.prevstate;
							it->state = t.state;

							// continue as there might be more, but we're done
							done = true;
						}

						// continue processing from here
						continue;
					}
				}
			}

			// if we've gone past where the new subresource range would sit
			if(it->range.aspectMask > t.range.aspectMask)
				break;

			// otherwise continue to try and find the subresource range
		}

		if(!done)
			RDCERR("Couldn't find subresource range to apply transition to - invalid!");
	}
}

void VulkanResourceManager::Hack_PropagateReferencesToMemory()
{
	// very nasty - prevents us re-processing the same entries when we loop
	// around to recursively propagate references.
	std::set<ResourceId> processed;

	// VKTODOMED this is hack, should be done earlier, but for now it works.
	// iterate through every referenced resource and make sure its memory is referenced
	// too.
	for(auto it = m_FrameReferencedResources.begin(); it != m_FrameReferencedResources.end(); ++it)
	{
		ResourceId id = it->first;

		if(processed.find(id) != processed.end()) continue;
		processed.insert(id);

		VkResourceRecord *record = GetResourceRecord(id);

		if(record && record->GetMemoryRecord())
		{
			RDCDEBUG("Propagating reference from %llu to %llu", record->GetResourceID(), record->GetMemoryRecord()->GetResourceID());
			// mark it as read-before-write so that we ensure there are initial states serialised for it.
			MarkResourceFrameReferenced(record->GetMemoryRecord()->GetResourceID(), eFrameRef_ReadBeforeWrite);
		}
		else if(record && HasCurrentResource(id))
		{
			// also extra hack - framebuffers and views and things need to mark their
			// parents referenced so that we can eventually come to the image or buffer
			// with a memory record.
			WrappedVkRes *res = GetCurrentResource(id);

			if(WrappedVkBufferView::IsAlloc(res) ||
					WrappedVkImageView::IsAlloc(res) ||
					WrappedVkFramebuffer::IsAlloc(res))
			{
				record->MarkParentsReferenced(this, eFrameRef_Read);

				RDCDEBUG("Propagating references to parents from %llu", record->GetResourceID());

				// reset to start so we can do this recursively - nasty I know.
				it = m_FrameReferencedResources.begin();
			}
		}
	}
}

bool VulkanResourceManager::Force_InitialState(WrappedVkRes *res)
{
	return false;
}

bool VulkanResourceManager::Need_InitialStateChunk(WrappedVkRes *res)
{
	return true;
}

bool VulkanResourceManager::Prepare_InitialState(WrappedVkRes *res)
{
	return m_Core->Prepare_InitialState(res);
}

bool VulkanResourceManager::Serialise_InitialState(WrappedVkRes *res)
{
	return m_Core->Serialise_InitialState(res);
}

void VulkanResourceManager::Create_InitialState(ResourceId id, WrappedVkRes *live, bool hasData)
{
	return m_Core->Create_InitialState(id, live, hasData);
}

void VulkanResourceManager::Apply_InitialState(WrappedVkRes *live, InitialContentData initial)
{
	return m_Core->Apply_InitialState(live, initial);
}

bool VulkanResourceManager::ResourceTypeRelease(WrappedVkRes *res)
{
	bool ret = m_Core->ReleaseResource(res);
	ReleaseWrappedResource(res);
	return ret;
}
