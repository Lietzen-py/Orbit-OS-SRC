/*
 * Copyright 2003-2005, Axel Dörfler, axeld@pinc-software.de. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef _KERNEL_IMAGE_H
#define _KERNEL_IMAGE_H


#include <image.h>

#include <image_defs.h>


struct image;

namespace BKernel {
	struct Team;
}

using BKernel::Team;


#ifdef __cplusplus

#include <util/DoublyLinkedList.h>
#include <util/OpenHashTable.h>

struct image : public DoublyLinkedListLinkImpl<struct image> {
	struct image*			hash_link;
	extended_image_info		info;
	team_id					team;
};

#endif	// __cplusplus

// image notifications
#define IMAGE_MONITOR	'_Im_'
#define IMAGE_ADDED		0x01
#define IMAGE_REMOVED	0x02


#ifdef __cplusplus
extern "C" {
#endif

extern image_id register_image(Team *team, extended_image_info *info,
					size_t size);
extern status_t unregister_image(Team *team, image_id id);
extern status_t copy_images(team_id fromTeamId, Team *toTeam);
extern int32 count_images(Team *team);
extern status_t remove_images(Team *team);

typedef bool (*image_iterator_callback)(struct image* image, void* cookie);
struct image* image_iterate_through_images(image_iterator_callback callback,
					void* cookie);
struct image* image_iterate_through_team_images(team_id teamID,
					image_iterator_callback callback, void* cookie);

extern status_t image_init(void);

// user-space exported calls
extern status_t _user_unregister_image(image_id id);
extern image_id _user_register_image(extended_image_info *userInfo,
					size_t size);
extern void		_user_image_relocated(image_id id);
extern void		_user_loading_app_failed(status_t error);
extern status_t _user_get_next_image_info(team_id team, int32 *_cookie,
					image_info *userInfo, size_t size);
extern status_t _user_get_image_info(image_id id, image_info *userInfo, size_t size);

#ifdef __cplusplus
}
#endif

#endif	/* _KRENEL_IMAGE_H */
