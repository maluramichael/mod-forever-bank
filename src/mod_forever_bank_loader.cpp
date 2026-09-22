/*
 * mod-forever-bank loader.
 *
 * The playerbots fork auto-globs every module's sources into one lib and
 * looks up a loader symbol derived from the folder name: for folder
 * "mod-forever-bank" that symbol is exactly "Addmod_forever_bankScripts". It
 * must exist and call our real registration function.
 *
 * Released under GNU GPL v2; redistribute/modify under version 2 of the
 * License, or (at your option) any later version.
 */

void AddForeverBankScripts();

void Addmod_forever_bankScripts()
{
    AddForeverBankScripts();
}
