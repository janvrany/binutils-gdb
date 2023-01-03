/* Handle ROCm Code Objects for GDB, the GNU Debugger.

   Copyright (C) 2019-2022 Free Software Foundation, Inc.

   This file is part of GDB.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.  */

#include "defs.h"

#include "amd-dbgapi-target.h"
#include "amdgpu-tdep.h"
#include "arch-utils.h"
#include "elf-bfd.h"
#include "elf/amdgpu.h"
#include "gdbsupport/fileio.h"
#include "inferior.h"
#include "observable.h"
#include "solib.h"
#include "solib-svr4.h"
#include "solist.h"
#include "symfile.h"

/* ROCm-specific inferior data.  */

struct solib_info
{
  /* List of code objects loaded into the inferior.  */
  so_list *solib_list;
};

/* Per-inferior data key.  */
static const registry<inferior>::key<solib_info> rocm_solib_data;

static target_so_ops rocm_solib_ops;

/* Free the solib linked list.  */

static void
rocm_free_solib_list (struct solib_info *info)
{
  while (info->solib_list != nullptr)
    {
      struct so_list *next = info->solib_list->next;

      free_so (info->solib_list);
      info->solib_list = next;
    }

  info->solib_list = nullptr;
}


/* Fetch the solib_info data for INF.  */

static struct solib_info *
get_solib_info (inferior *inf)
{
  solib_info *info = rocm_solib_data.get (inf);

  if (info == nullptr)
    info = rocm_solib_data.emplace (inf);

  return info;
}

/* Relocate section addresses.  */

static void
rocm_solib_relocate_section_addresses (struct so_list *so,
				       struct target_section *sec)
{
  if (!is_amdgpu_arch (gdbarch_from_bfd (so->abfd)))
    {
      svr4_so_ops.relocate_section_addresses (so, sec);
      return;
    }

  lm_info_svr4 *li = (lm_info_svr4 *) so->lm_info;
  sec->addr = sec->addr + li->l_addr;
  sec->endaddr = sec->endaddr + li->l_addr;
}

static void rocm_update_solib_list ();

static void
rocm_solib_handle_event ()
{
  /* Since we sit on top of svr4_so_ops, we might get called following an event
     concerning host libraries.  We must therefore forward the call.  If the
     event was for a ROCm code object, it will be a no-op.  On the other hand,
     if the event was for host libraries, rocm_update_solib_list will be
     essentially be a no-op (it will reload the same code object list as was
     previously loaded).  */
  svr4_so_ops.handle_event ();

  rocm_update_solib_list ();
}

/* Make a deep copy of the solib linked list.  */

static so_list *
rocm_solib_copy_list (const so_list *src)
{
  struct so_list *dst = nullptr;
  struct so_list **link = &dst;

  while (src != nullptr)
    {
      struct so_list *newobj;

      newobj = XNEW (struct so_list);
      memcpy (newobj, src, sizeof (struct so_list));

      lm_info_svr4 *src_li = (lm_info_svr4 *) src->lm_info;
      newobj->lm_info = new lm_info_svr4 (*src_li);

      newobj->next = nullptr;
      *link = newobj;
      link = &newobj->next;

      src = src->next;
    }

  return dst;
}

/* Build a list of `struct so_list' objects describing the shared
   objects currently loaded in the inferior.  */

static struct so_list *
rocm_solib_current_sos ()
{
  /* First, retrieve the host-side shared library list.  */
  so_list *head = svr4_so_ops.current_sos ();

  /* Then, the device-side shared library list.  */
  so_list *list = get_solib_info (current_inferior ())->solib_list;

  if (list == nullptr)
    return head;

  list = rocm_solib_copy_list (list);

  if (head == nullptr)
    return list;

  /* Append our libraries to the end of the list.  */
  so_list *tail;
  for (tail = head; tail->next; tail = tail->next)
    /* Nothing.  */;
  tail->next = list;

  return head;
}

namespace {

/* Interface to interact with a ROCm code object stream.  */

struct rocm_code_object_stream
{
  DISABLE_COPY_AND_ASSIGN (rocm_code_object_stream);

  /* Copy SIZE bytes from the underlying objfile storage starting at OFFSET
     into the user provided buffer BUF.

     Return the number of bytes actually copied (might be inferior to SIZE if
     the end of the stream is reached).  */
  virtual file_ptr read (void *buf, file_ptr size, file_ptr offset) = 0;

  /* Retrieve file information in SB.

     Return 0 on success.  On failure, set the appropriate bfd error number
     (using bfd_set_error) and return -1.  */
  int stat (struct stat *sb);

  virtual ~rocm_code_object_stream () = default;

protected:
  rocm_code_object_stream () = default;

  /* Return the size of the object file, or -1 if the size cannot be
     determined.

     This is a helper function for stat.  */
  virtual LONGEST size () = 0;
};

int
rocm_code_object_stream::stat (struct stat *sb)
{
  const LONGEST size = this->size ();
  if (size == -1)
    return -1;

  memset (sb, '\0', sizeof (struct stat));
  sb->st_size = size;
  return 0;
}

/* Interface to a ROCm object stream which is embedded in an ELF file
   accessible to the debugger.  */

struct rocm_code_object_stream_file final : rocm_code_object_stream
{
  DISABLE_COPY_AND_ASSIGN (rocm_code_object_stream_file);

  rocm_code_object_stream_file (int fd, ULONGEST offset, ULONGEST size);

  file_ptr read (void *buf, file_ptr size, file_ptr offset) override;

  LONGEST size () override;

  ~rocm_code_object_stream_file () override;

protected:

  /* The target file descriptor for this stream.  */
  int m_fd;

  /* The offset of the ELF file image in the target file.  */
  ULONGEST m_offset;

  /* The size of the ELF file image.  The value 0 means that it was
     unspecified in the URI descriptor.  */
  ULONGEST m_size;
};

rocm_code_object_stream_file::rocm_code_object_stream_file
  (int fd, ULONGEST offset, ULONGEST size)
  : m_fd (fd), m_offset (offset), m_size (size)
{
}

file_ptr
rocm_code_object_stream_file::read (void *buf, file_ptr size,
				    file_ptr offset)
{
  fileio_error target_errno;
  file_ptr nbytes = 0;
  while (size > 0)
    {
      QUIT;

      file_ptr bytes_read
	= target_fileio_pread (m_fd, static_cast<gdb_byte *> (buf) + nbytes,
			       size, m_offset + offset + nbytes,
			       &target_errno);

      if (bytes_read == 0)
	break;

      if (bytes_read < 0)
	{
	  errno = fileio_error_to_host (target_errno);
	  bfd_set_error (bfd_error_system_call);
	  return -1;
	}

      nbytes += bytes_read;
      size -= bytes_read;
    }

  return nbytes;
}

LONGEST
rocm_code_object_stream_file::size ()
{
  if (m_size == 0)
    {
      fileio_error target_errno;
      struct stat stat;
      if (target_fileio_fstat (m_fd, &stat, &target_errno) < 0)
	{
	  errno = fileio_error_to_host (target_errno);
	  bfd_set_error (bfd_error_system_call);
	  return -1;
	}

      /* Check that the offset is valid.  */
      if (m_offset >= stat.st_size)
	{
	  bfd_set_error (bfd_error_bad_value);
	  return -1;
	}

      m_size = stat.st_size - m_offset;
    }

  return m_size;
}

rocm_code_object_stream_file::~rocm_code_object_stream_file ()
{
  fileio_error target_errno;
  target_fileio_close (m_fd, &target_errno);
}

/* Interface to a code object which lives in the inferior's memory.  */

struct rocm_code_object_stream_memory final : public rocm_code_object_stream
{
  DISABLE_COPY_AND_ASSIGN (rocm_code_object_stream_memory);

  rocm_code_object_stream_memory (gdb::byte_vector buffer);

  file_ptr read (void *buf, file_ptr size, file_ptr offset) override;

protected:

  /* Snapshot of the original ELF image taken during load.  This is done to
     support the situation where an inferior uses an in-memory image, and
     releases or re-uses this memory before GDB is done using it.  */
  gdb::byte_vector m_objfile_image;

  LONGEST size () override
  {
    return m_objfile_image.size ();
  }
};

rocm_code_object_stream_memory::rocm_code_object_stream_memory
  (gdb::byte_vector buffer)
  : m_objfile_image (std::move (buffer))
{
}

file_ptr
rocm_code_object_stream_memory::read (void *buf, file_ptr size,
				      file_ptr offset)
{
  if (size > m_objfile_image.size () - offset)
    size = m_objfile_image.size () - offset;

  memcpy (buf, m_objfile_image.data () + offset, size);
  return size;
}

} /* anonymous namespace */

static void *
rocm_bfd_iovec_open (bfd *abfd, void *inferior_void)
{
  gdb::string_view uri (bfd_get_filename (abfd));
  gdb::string_view protocol_delim = "://";
  size_t protocol_end = uri.find (protocol_delim);
  std::string protocol = gdb::to_string (uri.substr (0, protocol_end));
  protocol_end += protocol_delim.length ();

  std::transform (protocol.begin (), protocol.end (), protocol.begin (),
		  [] (unsigned char c) { return std::tolower (c); });

  gdb::string_view path;
  size_t path_end = uri.find_first_of ("#?", protocol_end);
  if (path_end != std::string::npos)
    path = uri.substr (protocol_end, path_end++ - protocol_end);
  else
    path = uri.substr (protocol_end);

  /* %-decode the string.  */
  std::string decoded_path;
  decoded_path.reserve (path.length ());
  for (size_t i = 0; i < path.length (); ++i)
    if (path[i] == '%'
	&& i < path.length () - 2
	&& std::isxdigit (path[i + 1])
	&& std::isxdigit (path[i + 2]))
      {
	gdb::string_view hex_digits = path.substr (i + 1, 2);
	decoded_path += std::stoi (gdb::to_string (hex_digits), 0, 16);
	i += 2;
      }
    else
      decoded_path += path[i];

  /* Tokenize the query/fragment.  */
  std::vector<gdb::string_view> tokens;
  size_t pos, last = path_end;
  while ((pos = uri.find ('&', last)) != std::string::npos)
    {
      tokens.emplace_back (uri.substr (last, pos - last));
      last = pos + 1;
    }

  if (last != std::string::npos)
    tokens.emplace_back (uri.substr (last));

  /* Create a tag-value map from the tokenized query/fragment.  */
  std::unordered_map<gdb::string_view, gdb::string_view,
		     gdb::string_view_hash> params;
  for (gdb::string_view token : tokens)
    {
      size_t delim = token.find ('=');
      if (delim != std::string::npos)
	{
	  gdb::string_view tag = token.substr (0, delim);
	  gdb::string_view val = token.substr (delim + 1);
	  params.emplace (tag, val);
	}
    }

  try
    {
      ULONGEST offset = 0;
      ULONGEST size = 0;
      inferior *inferior = static_cast<struct inferior *> (inferior_void);

      auto try_strtoulst = [] (gdb::string_view v)
	{
	  errno = 0;
	  ULONGEST value = strtoulst (v.data (), nullptr, 0);
	  if (errno != 0)
	    {
	      /* The actual message doesn't matter, the exception is caught
	         below, transformed in a BFD error, and the message is lost.  */
	      error (_("Failed to parse integer."));
	    }

	  return value;
	};

      auto offset_it = params.find ("offset");
      if (offset_it != params.end ())
	offset = try_strtoulst (offset_it->second);

      auto size_it = params.find ("size");
      if (size_it != params.end ())
	{
	  size = try_strtoulst (size_it->second);
	  if (size == 0)
	    error (_("Invalid size value"));
	}

      if (protocol == "file")
	{
	  fileio_error target_errno;
	  int fd
	    = target_fileio_open (static_cast<struct inferior *> (inferior),
				  decoded_path.c_str (), FILEIO_O_RDONLY,
				  false, 0, &target_errno);

	  if (fd == -1)
	    {
	      errno = fileio_error_to_host (target_errno);
	      bfd_set_error (bfd_error_system_call);
	      return nullptr;
	    }

	  return new rocm_code_object_stream_file (fd, offset, size);
	}

      if (protocol == "memory")
	{
	  ULONGEST pid = try_strtoulst (path);
	  if (pid != inferior->pid)
	    {
	      warning (_("`%s': code object is from another inferior"),
		       gdb::to_string (uri).c_str ());
	      bfd_set_error (bfd_error_bad_value);
	      return nullptr;
	    }

	  gdb::byte_vector buffer (size);
	  if (target_read_memory (offset, buffer.data (), size) != 0)
	    {
	      warning (_("Failed to copy the code object from the inferior"));
	      bfd_set_error (bfd_error_bad_value);
	      return nullptr;
	    }

	  return new rocm_code_object_stream_memory (std::move (buffer));
	}

      warning (_("`%s': protocol not supported: %s"),
	       gdb::to_string (uri).c_str (), protocol.c_str ());
      bfd_set_error (bfd_error_bad_value);
      return nullptr;
    }
  catch (const gdb_exception_quit &ex)
    {
      set_quit_flag ();
      bfd_set_error (bfd_error_bad_value);
      return nullptr;
    }
  catch (const gdb_exception &ex)
    {
      bfd_set_error (bfd_error_bad_value);
      return nullptr;
    }
}

static int
rocm_bfd_iovec_close (bfd *nbfd, void *data)
{
  delete static_cast<rocm_code_object_stream *> (data);

  return 0;
}

static file_ptr
rocm_bfd_iovec_pread (bfd *abfd, void *data, void *buf, file_ptr size,
		      file_ptr offset)
{
  return static_cast<rocm_code_object_stream *> (data)->read (buf, size,
							      offset);
}

static int
rocm_bfd_iovec_stat (bfd *abfd, void *data, struct stat *sb)
{
  return static_cast<rocm_code_object_stream *> (data)->stat (sb);
}

static gdb_bfd_ref_ptr
rocm_solib_bfd_open (const char *pathname)
{
  /* Handle regular files with SVR4 open.  */
  if (strstr (pathname, "://") == nullptr)
    return svr4_so_ops.bfd_open (pathname);

  gdb_bfd_ref_ptr abfd
    = gdb_bfd_openr_iovec (pathname, "elf64-amdgcn", rocm_bfd_iovec_open,
			   current_inferior (), rocm_bfd_iovec_pread,
			   rocm_bfd_iovec_close, rocm_bfd_iovec_stat);

  if (abfd == nullptr)
    error (_("Could not open `%s' as an executable file: %s"), pathname,
	   bfd_errmsg (bfd_get_error ()));

  /* Check bfd format.  */
  if (!bfd_check_format (abfd.get (), bfd_object))
    error (_("`%s': not in executable format: %s"),
	   bfd_get_filename (abfd.get ()), bfd_errmsg (bfd_get_error ()));

  unsigned char osabi = elf_elfheader (abfd)->e_ident[EI_OSABI];
  unsigned char osabiversion = elf_elfheader (abfd)->e_ident[EI_ABIVERSION];

  /* Check that the code object is using the HSA OS ABI.  */
  if (osabi != ELFOSABI_AMDGPU_HSA)
    error (_("`%s': ELF file OS ABI is not supported (%d)."),
	   bfd_get_filename (abfd.get ()), osabi);

  /* We support HSA code objects V3 and greater.  */
  if (osabiversion < ELFABIVERSION_AMDGPU_HSA_V3)
    error (_("`%s': ELF file HSA OS ABI version is not supported (%d)."),
	   bfd_get_filename (abfd.get ()), osabiversion);

  return abfd;
}

static void
rocm_solib_create_inferior_hook (int from_tty)
{
  rocm_free_solib_list (get_solib_info (current_inferior ()));

  svr4_so_ops.solib_create_inferior_hook (from_tty);
}

static void
rocm_update_solib_list ()
{
  inferior *inf = current_inferior ();

  amd_dbgapi_process_id_t process_id = get_amd_dbgapi_process_id (inf);
  if (process_id.handle == AMD_DBGAPI_PROCESS_NONE.handle)
    return;

  solib_info *info = get_solib_info (inf);

  rocm_free_solib_list (info);
  struct so_list **link = &info->solib_list;

  amd_dbgapi_code_object_id_t *code_object_list;
  size_t count;

  amd_dbgapi_status_t status
    = amd_dbgapi_process_code_object_list (process_id, &count,
					   &code_object_list, nullptr);
  if (status != AMD_DBGAPI_STATUS_SUCCESS)
    {
      warning (_("amd_dbgapi_process_code_object_list failed (%s)"),
	       get_status_string (status));
      return;
    }

  for (size_t i = 0; i < count; ++i)
    {
      CORE_ADDR l_addr;
      char *uri_bytes;

      status = amd_dbgapi_code_object_get_info
	(code_object_list[i], AMD_DBGAPI_CODE_OBJECT_INFO_LOAD_ADDRESS,
	 sizeof (l_addr), &l_addr);
      if (status != AMD_DBGAPI_STATUS_SUCCESS)
	continue;

      status = amd_dbgapi_code_object_get_info
	(code_object_list[i], AMD_DBGAPI_CODE_OBJECT_INFO_URI_NAME,
	 sizeof (uri_bytes), &uri_bytes);
      if (status != AMD_DBGAPI_STATUS_SUCCESS)
	continue;

      struct so_list *so = XCNEW (struct so_list);
      lm_info_svr4 *li = new lm_info_svr4;
      li->l_addr = l_addr;
      so->lm_info = li;

      strncpy (so->so_name, uri_bytes, sizeof (so->so_name));
      so->so_name[sizeof (so->so_name) - 1] = '\0';
      xfree (uri_bytes);

      /* Make so_original_name unique so that code objects with the same URI
	 but different load addresses are seen by gdb core as different shared
	 objects.  */
      xsnprintf (so->so_original_name, sizeof (so->so_original_name),
		 "code_object_%ld", code_object_list[i].handle);

      so->next = nullptr;
      *link = so;
      link = &so->next;
    }

  xfree (code_object_list);

  if (rocm_solib_ops.current_sos == NULL)
    {
      /* Override what we need to.  */
      rocm_solib_ops = svr4_so_ops;
      rocm_solib_ops.current_sos = rocm_solib_current_sos;
      rocm_solib_ops.solib_create_inferior_hook
	= rocm_solib_create_inferior_hook;
      rocm_solib_ops.bfd_open = rocm_solib_bfd_open;
      rocm_solib_ops.relocate_section_addresses
	= rocm_solib_relocate_section_addresses;
      rocm_solib_ops.handle_event = rocm_solib_handle_event;

      /* Engage the ROCm so_ops.  */
      set_gdbarch_so_ops (current_inferior ()->gdbarch, &rocm_solib_ops);
    }
}

static void
rocm_solib_target_inferior_created (inferior *inf)
{
  rocm_free_solib_list (get_solib_info (inf));
  rocm_update_solib_list ();

  /* Force GDB to reload the solibs.  */
  current_inferior ()->pspace->clear_solib_cache ();
  solib_add (nullptr, 0, auto_solib_add);
}

/* -Wmissing-prototypes */
extern initialize_file_ftype _initialize_rocm_solib;

void
_initialize_rocm_solib ()
{
  /* The dependency on the amd-dbgapi exists because solib-rocm's
     inferior_created observer needs amd-dbgapi to have attached the process,
     which happens in amd_dbgapi_target's inferior_created observer.  */
  gdb::observers::inferior_created.attach
    (rocm_solib_target_inferior_created,
     "solib-rocm",
     { &get_amd_dbgapi_target_inferior_created_observer_token () });
}
