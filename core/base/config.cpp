#include "config.h"

bool operator< (const Key &l, const Key &r)
{
	if(l.type != r.type)
	{
		return l.type < r.type;
	}

	switch(l.type)
	{
	case Key::NUMBER:
		return l.Num() < r.Num();
	case Key::STRING:
		return l.Str() < r.Str();
	}
	//should never come here
	throw ConfigException(KEY_TYPE_INVALID);
	return false;
}

const Value& Value::operator[](const Key &k) const
{
	if(!IsTab())
	{
		throw ConfigException(VALUE_EXCEPT_TABLE);
	}

	return tab[k];
}

const Value& Table::operator[] (const Key &k) const
{
	EntryMap::const_iterator cit = entry_map.find(k);
	if(cit == entry_map.end())
	{
		throw ConfigException(TABLE_ENTRY_NOT_FOUND);
	}

	return *(cit->second);
}

void Value::Insert(const Key &k, const Value &v)
{
	if(!IsTab())
	{
		throw ConfigException(VALUE_EXCEPT_TABLE);
	}

	tab.entry_map[k] = std::shared_ptr<Value>(new Value(v));
}

void Config::Load(std::shared_ptr<lua_State> sp, Value &value)
{
	lua_State *L = sp.get();
	lua_pushnil(L);
	while(lua_next(L, -2) != 0)
	{
		Key k;
		Value v;
		int tkey = lua_type(L, -2);
		switch(tkey)
		{
		case LUA_TSTRING:
			k = lua_tostring(L, -2);
			break;
		case LUA_TNUMBER:
			k = lua_tonumber(L, -2);
			break;
		default:
			throw ConfigException(LUA_KEY_INVALID);
			break;
		};

		int tvalue = lua_type(L, -1);
		switch(tvalue)
		{
		case LUA_TSTRING:
			v = lua_tostring(L, -1);
			break;
		case LUA_TNUMBER:
			v = lua_tonumber(L, -1);
			break;
		case LUA_TTABLE:
			v = Value::TabVal();
			Load(sp, v);
			break;
		default:
			throw ConfigException(LUA_VALUE_INVALID);
			break;
		}
		value.Insert(k, v);
		lua_pop(L, 1);
	}
}

/*
int main()
{
	Config config("config.lua");
	std::cout << config[2][3].Str() << std::endl;
	return 0;
}

config.lua

config = {
	aa = "123",
	bb = "1231",
	[1] = 123,
	[2] = { "123" , "234" , "adsf"},
}
*/
