file\_model
===========
The `file_model` class allows to perform the following operations on regular files and block devices:

* Modify data.
* Add data (not allowed for block devices).
* Delete data (not allowed for block devices).
* Get data.
* Undo changes.
* Redo changes.
* Search forward.
* Search backward.

The files to be modified can be bigger than the available memory, as only the portions of the file which have been changed are stored in memory. The file in disk is not modified until the `save()` method is called.

The file is handled internally as a linked list of blocks, each block points to data which is either in memory or in disk.

The class `trivial_file_model` exports the same methods as the `file_model` class but the file on disk is updated after every change. This class has been used for testing that the `file_model` class works correctly.
