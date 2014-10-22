import random
import string

f = open('fileIn','w')

for cont in range(800):
 for cont2 in range(100):
  f.write(random.choice(string.letters))
 f.write('\n')

f.close()
