#include "ir/debug_info.h"

#include "ir/node.h"

namespace rat {
	DebugType* DebugInfo::makeType(DebugType::Kind kind, String name, U32 size) {
		types.push_back(std::make_unique<DebugType>());
		DebugType* t = types.back().get();
		t->kind = kind;
		t->name = std::move(name);
		t->size = size;
		return t;
	}

	// consecutive nodes mostly share a location, reuse the last entry then
	U32 DebugBody::addLoc(SrcLoc src, U32 scope) {
		const DebugLoc& last = locs.back();
		B32 sameSrc = last.src.file == src.file && last.src.line == src.line && last.src.col == src.col;
		if(locs.size() <= 1 || last.scope != scope || !sameSrc)
			locs.push_back({src, scope});
		return (U32)locs.size() - 1;
	}
	// a constant keeps its value wherever it sits, so it needs no link
	void DebugBody::link(Node* n, U32 binding) {
		if(!n || isa<ConstantNode>(n))
			return;
		U32 id = n->getId();
		if(id >= refHead.size())
			refHead.resize(std::max(id + 1, (U32)refHead.size() * 2), 0);
		refs.push_back({binding, refHead[id]});
		refHead[id] = (U32)refs.size();
	}

	void DebugBody::bind(U32 inst, Node* value, Node* anchor, Node* after) {
		U32 id = (U32)bindings.size();
		bindings.push_back({inst, value, anchor, after});
		link(value, id);
		link(anchor, id);
		link(after, id);
	}

	void DebugBody::mergeArm(Node* arm, Node* to) {
		U32 id = arm->getId();
		if(id < refHead.size())
			for(U32 r = refHead[id]; r; r = refs[r - 1].next) {
				DebugBinding& b = bindings[refs[r - 1].binding];
				if(b.anchor == arm)
					b.value = nullptr;
			}
		retarget(arm, to);
	}

	void DebugBody::retarget(Node* from, Node* to) {
		U32 id = from->getId();
		if(id >= refHead.size())
			return;
		U32 r = refHead[id];
		refHead[id] = 0;
		for(; r; r = refs[r - 1].next) {
			U32 binding = refs[r - 1].binding;
			DebugBinding& b = bindings[binding];
			B32 moved = false;
			for(Node** slot : {&b.value, &b.anchor, &b.after})
				if(*slot == from) {
					*slot = to;
					moved = true;
				}
			if(moved)
				link(to, binding);
		}
	}
} // namespace rat
