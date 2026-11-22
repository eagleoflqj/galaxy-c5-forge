import importlib.util
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('time_client',ROOT/'scripts/patch-c5-time-genoff.py');m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)


class TimeClientTests(unittest.TestCase):
    def fixture(self):
        data=bytearray(0x1000)
        for flags,call in m.SITES:
            struct.pack_into('<I',data,flags,m.OLD)
            struct.pack_into('<I',data,call,0x94000000|(((m.SEND_PLT-call)//4)&0x3ffffff))
        return bytes(data)

    def test_exact_delta(self):
        before=self.fixture();after=m.replace_verified_words(before)
        for offset,_ in m.SITES:
            self.assertEqual(struct.unpack_from('<I',after,offset)[0],m.NEW)
        self.assertEqual(len(after),len(before))
        self.assertEqual(before,self.fixture())

    def test_wrong_flags_rejected(self):
        data=bytearray(self.fixture());data[m.SITES[0][0]]=0
        with self.assertRaises(ValueError):m.replace_verified_words(data)

    def test_wrong_call_rejected(self):
        data=bytearray(self.fixture());struct.pack_into('<I',data,m.SITES[1][1],0x94000000)
        with self.assertRaises(ValueError):m.replace_verified_words(data)

    def test_unverified_blob_rejected(self):
        with self.assertRaises(ValueError):m.transform(self.fixture())

    def test_closed_peer_semantics(self):
        source=r'''
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
int main(void) {
 for (int flags=0; flags<=1; flags++) {
  int pair[2]; assert(socketpair(AF_UNIX,SOCK_STREAM,0,pair)==0);
  close(pair[1]); pid_t pid=fork(); assert(pid>=0);
  if (!pid) {
   assert(signal(SIGPIPE,SIG_DFL)!=SIG_ERR); errno=0;
   ssize_t n=send(pair[0],"x",1,flags?MSG_NOSIGNAL:0);
   _exit(n==-1 && errno==EPIPE ? 42:43);
  }
  int status; assert(waitpid(pid,&status,0)==pid);close(pair[0]);
  if (flags) assert(WIFEXITED(status)&&WEXITSTATUS(status)==42);
  else assert(WIFSIGNALED(status)&&WTERMSIG(status)==SIGPIPE);
 }
 int pair[2];char data=0;assert(socketpair(AF_UNIX,SOCK_STREAM,0,pair)==0);
 assert(send(pair[0],"x",1,MSG_NOSIGNAL)==1);assert(recv(pair[1],&data,1,0)==1&&data=='x');close(pair[0]);close(pair[1]);return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='c5-time-socket-') as directory:
            path=Path(directory);(path/'test.c').write_text(source)
            run=subprocess.run(['cc','-Wall','-Wextra','-Werror',str(path/'test.c'),'-o',str(path/'test')],capture_output=True,timeout=15)
            self.assertEqual(run.returncode,0,run.stderr.decode())
            run=subprocess.run([str(path/'test')],capture_output=True,timeout=5)
            self.assertEqual(run.returncode,0,run.stderr.decode())


if __name__=='__main__':unittest.main()
