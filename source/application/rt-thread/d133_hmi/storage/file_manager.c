#include "file_manager.h"
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <dfs_fs.h>
#include <unistd.h>
/* 所有函数仅供存储/OTA 工作线程调用，禁止从 LVGL 回调直接调用。 */
int file_manager_list(const char *path, fm_list_t *out)
{
    DIR *dir; struct dirent *e; struct stat st; char full[FM_PATH_MAX];
    if (!path || !out) return -1;
    memset(out,0,sizeof(*out)); dir=opendir(path); if(!dir) return -2;
    while((e=readdir(dir))!=NULL) {
        if(!strcmp(e->d_name,".") || !strcmp(e->d_name,"..")) continue;
        if(out->count==FM_LIST_MAX) {out->truncated=1; break;}
        if(snprintf(full,sizeof(full),"%s/%s",path,e->d_name)>=(int)sizeof(full)) {out->truncated=1;continue;}
        if(stat(full,&st)) continue;
        fm_entry_t *v=&out->entries[out->count++];
        snprintf(v->name,sizeof(v->name),"%s",e->d_name);
        v->directory=S_ISDIR(st.st_mode); v->size=st.st_size;
    }
    closedir(dir); return 0;
}
int file_manager_read(const char *p,void *b,size_t n,size_t *got)
{ FILE *f; if(!p||!b||!got)return -1; f=fopen(p,"rb");if(!f)return -2; *got=fread(b,1,n,f);int r=ferror(f)?-3:0;fclose(f);return r; }
int file_manager_write(const char *p,const void *b,size_t n)
{ FILE *f;if(!p||(!b&&n))return -1;f=fopen(p,"wb");if(!f)return -2;int r=fwrite(b,1,n,f)==n?0:-3;if(fclose(f)&&!r)r=-3;return r; }
int file_manager_copy(const char *a,const char *b)
{
    char buf[2048];size_t n;int r=0;FILE *in,*out;
    if(!a||!b||!strcmp(a,b))return -1;
    in=fopen(a,"rb");if(!in)return -2;
    /* 使用独占创建，复制失败不覆盖用户已有文件。 */
    out=fopen(b,"rb");if(out){fclose(out);fclose(in);return -4;}
    out=fopen(b,"wb");if(!out){fclose(in);return -2;}
    while((n=fread(buf,1,sizeof(buf),in))>0)if(fwrite(buf,1,n,out)!=n){r=-3;break;}
    if(ferror(in))r=-3;
    fclose(in);
    if(fclose(out))r=-3;
    return r;
}
int file_manager_delete(const char *p){return p?remove(p):-1;}
int file_manager_mkdir(const char *p){return p?mkdir(p,0777):-1;}
int file_manager_rename(const char *a,const char *b){return a&&b?rename(a,b):-1;}
int file_manager_size(const char *p,uint32_t *n){struct stat s;if(!p||!n||stat(p,&s))return -1;*n=s.st_size;return 0;}
int file_manager_capacity(const char *p,uint64_t *total,uint64_t *avail)
{struct statfs s;if(!p||!total||!avail||dfs_statfs(p,&s))return -1;*total=(uint64_t)s.f_blocks*s.f_bsize;*avail=(uint64_t)s.f_bfree*s.f_bsize;return 0;}

