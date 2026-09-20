#pragma once
#include "aurora_shim.h"
#include "../profile_data.h"
#ifdef __cplusplus
extern "C" {
#endif
void aushim_profile_init(void);
void aushim_profile_update(void);
void aushim_profile_open(int open);
int aushim_profile_capturing(void);
int aushim_profile_typing(void);
int aushim_profile_test_ready(void);
AUSHIM_API int aushim_profile_back(void);
AUSHIM_API void aushim_profile_bind(void (*changed)(const char*,const unsigned char*,unsigned,const char*));
AUSHIM_API int aushim_profile_receive(const char* hash,const unsigned char* pixels,unsigned size);
#ifdef __cplusplus
}
#include <string>
#include <vector>
struct ProfileMenuView {
 int row=0; bool editing=false, picking=false;
 unsigned revision=0;
 std::string name, message;
 std::vector<unsigned char> pixels;
};
const ProfileMenuView& profile_menu_view();
bool profile_avatar(const char* hash,std::vector<unsigned char>& pixels);
#endif
