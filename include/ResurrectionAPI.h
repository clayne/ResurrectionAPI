#pragma once

class ResurrectionAPI
{
public:
	virtual bool should_resurrect(RE::Actor*) const { return false; };
	virtual void resurrect(RE::Actor*){};
};
