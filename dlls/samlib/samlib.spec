@ stub SamAddMemberToAlias
@ stub SamAddMemberToGroup
@ stub SamAddMultipleMembersToAlias
@ stub SamChangePasswordUser2
@ stub SamChangePasswordUser3
@ stub SamChangePasswordUser
@ stdcall SamCloseHandle(ptr)
@ stdcall SamConnect(ptr ptr long ptr)
@ stub SamConnectWithCreds
@ stub SamCreateAliasInDomain
@ stub SamCreateGroupInDomain
@ stub SamCreateUser2InDomain
@ stub SamCreateUserInDomain
@ stub SamDeleteAlias
@ stub SamDeleteGroup
@ stub SamDeleteUser
@ stdcall SamEnumerateAliasesInDomain(ptr ptr ptr long ptr)
@ stub SamEnumerateDomainsInSamServer
@ stub SamEnumerateGroupsInDomain
@ stdcall SamEnumerateUsersInDomain(ptr ptr long ptr long ptr)
@ stdcall SamFreeMemory(ptr)
@ stub SamGetAliasMembership
@ stub SamGetCompatibilityMode
@ stub SamGetDisplayEnumerationIndex
@ stub SamGetGroupsForUser
@ stdcall SamGetMembersInAlias(ptr ptr ptr)
@ stub SamGetMembersInGroup
@ stdcall SamLookupDomainInSamServer(ptr ptr ptr)
@ stdcall SamLookupIdsInDomain(ptr long ptr ptr ptr)
@ stdcall SamLookupNamesInDomain(ptr long ptr ptr ptr)
@ stdcall SamOpenAlias(ptr long long ptr)
@ stdcall SamOpenDomain(ptr long ptr ptr)
@ stub SamOpenGroup
@ stdcall SamOpenUser(ptr long long ptr)
@ stub SamQueryDisplayInformation
@ stdcall SamQueryInformationAlias(ptr long ptr)
@ stub SamQueryInformationDomain
@ stub SamQueryInformationGroup
@ stdcall SamQueryInformationUser(ptr long ptr)
@ stub SamQuerySecurityObject
@ stub SamRemoveMemberFromAlias
@ stub SamRemoveMemberFromForeignDomain
@ stub SamRemoveMemberFromGroup
@ stub SamRemoveMultipleMembersFromAlias
@ stdcall SamRidToSid(ptr long ptr)
@ stub SamSetInformationAlias
@ stub SamSetInformationDomain
@ stub SamSetInformationGroup
@ stub SamSetInformationUser
@ stub SamSetMemberAttributesOfGroup
@ stub SamSetSecurityObject
@ stub SamShutdownSamServer
@ stub SamTestPrivateFunctionsDomain
@ stub SamTestPrivateFunctionsUser
@ stub SamiChangeKeys
@ stub SamiChangePasswordUser2
@ stub SamiChangePasswordUser3
@ stub SamiChangePasswordUser
@ stub SamiEncryptPasswords
@ stub SamiGetBootKeyInformation
@ stub SamiLmChangePasswordUser
@ stub SamiOemChangePasswordUser2
@ stub SamiSetBootKeyInformation
@ stub SamiSetDSRMPassword
@ stub SamiSetDSRMPasswordOWF
@ stdcall SamRegisterObjectChangeNotification(long ptr)
@ stdcall SamUnregisterObjectChangeNotification(long ptr)
