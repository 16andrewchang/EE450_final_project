Extra Credit:
My project uses a XOR-based symmetric encryption algorithm.
The key idea is to apply a bitwise XOR operation between each character of the plaintext and a key.

The encryption key is a 16-byte constant:
XOR_KEY ="\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF"
The algorithm loops over each character in the input string and XOR it with a byte from the key.
It compares each bit:
* If the bits are different, the result is 1.
* If the bits are the same, the result is 0.

-Example of how XOR encryption works:
Let's say the original text is "Alice", which is:"/0x41/0x6C/0x69/0x63/0x65"
We use the first 5 bytes of the XOR_KEY, which are all 0xFF, and the result is:
"\xBE\x93\x96\x9C\x9A".
To decrypt it, just perform a bitwise XOR operation between the encrypted string and the encryption key.

-To run the program using XOR encryption protocol, you need to add "XOR" to the end of any operations.
For example:
./client Chinmay XOR
./client Alice Chinamy 100 XOR
./monitor TXLIST XOR