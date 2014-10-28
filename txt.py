import random
import string

f = open('fileInB','w')

for cont in range(80000):
 for cont2 in range(100):
  f.write(random.choice(string.letters))
 f.write('\n')

f.close()
