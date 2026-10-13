@ stub NetGetDisplayInformationIndex
@ stub NetGroupAdd
@ stdcall NetGroupAddUser(wstr wstr wstr) netapi32.NetGroupAddUser
@ stub NetGroupDel
@ stub NetGroupDelUser
@ stdcall NetGroupEnum(wstr long ptr long ptr ptr ptr) netapi32.NetGroupEnum
@ stdcall NetGroupGetInfo(wstr wstr long ptr) netapi32.NetGroupGetInfo
@ stub NetGroupGetUsers
@ stub NetGroupSetInfo
@ stub NetGroupSetUsers
@ stdcall NetLocalGroupAdd(wstr long ptr ptr) netapi32.NetLocalGroupAdd
@ stdcall NetLocalGroupAddMember(wstr wstr ptr) netapi32.NetLocalGroupAddMember
@ stdcall NetLocalGroupAddMembers(wstr wstr long ptr long) netapi32.NetLocalGroupAddMembers
@ stdcall NetLocalGroupDel(wstr wstr) netapi32.NetLocalGroupDel
@ stdcall NetLocalGroupDelMember(wstr wstr ptr) netapi32.NetLocalGroupDelMember
@ stdcall NetLocalGroupDelMembers(wstr wstr long ptr long) netapi32.NetLocalGroupDelMembers
@ stdcall NetLocalGroupEnum(wstr long ptr long ptr ptr ptr) netapi32.NetLocalGroupEnum
@ stdcall NetLocalGroupGetInfo(wstr wstr long ptr) netapi32.NetLocalGroupGetInfo
@ stdcall NetLocalGroupGetMembers(wstr wstr long ptr long ptr ptr ptr) netapi32.NetLocalGroupGetMembers
@ stdcall NetLocalGroupSetInfo(wstr wstr long ptr ptr) netapi32.NetLocalGroupSetInfo
@ stdcall NetLocalGroupSetMembers(wstr wstr long ptr long) netapi32.NetLocalGroupSetMembers
@ stdcall NetQueryDisplayInformation(wstr long long long long ptr ptr) netapi32.NetQueryDisplayInformation
@ stdcall NetUserAdd(wstr long ptr ptr) netapi32.NetUserAdd
@ stdcall NetUserChangePassword(wstr wstr wstr wstr) netapi32.NetUserChangePassword
@ stdcall NetUserDel(wstr wstr) netapi32.NetUserDel
@ stdcall NetUserEnum(wstr long long ptr long ptr ptr ptr) netapi32.NetUserEnum
@ stdcall NetUserGetGroups(wstr wstr long ptr long ptr ptr) netapi32.NetUserGetGroups
@ stdcall NetUserGetInfo(wstr wstr long ptr) netapi32.NetUserGetInfo
@ stdcall NetUserGetLocalGroups(wstr wstr long long ptr long ptr ptr) netapi32.NetUserGetLocalGroups
@ stdcall NetUserModalsGet(wstr long ptr) netapi32.NetUserModalsGet
@ stub NetUserModalsSet
@ stub NetUserSetGroups
@ stub NetUserSetInfo
@ stdcall NetValidatePasswordPolicy(wstr ptr long ptr ptr) netapi32.NetValidatePasswordPolicy
@ stdcall NetValidatePasswordPolicyFree(ptr) netapi32.NetValidatePasswordPolicyFree
